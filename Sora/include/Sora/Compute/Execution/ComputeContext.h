/**
 * @file ComputeContext.h
 * @brief Owning heterogeneous runtime context and user-facing logical graph
 * submission.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/ArtifactCache.h>
#include <Sora/Compute/CompilerService.h>
#include <Sora/Compute/Execution/RecoveringSubmitSender.h>
#include <Sora/Compute/PipelineService.h>
#include <Sora/Compute/Runtime/GraphOptimizer.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>
#include <Sora/Compute/Runtime/PhysicalGraphMaterializer.h>
#include <Sora/Compute/Runtime/PortablePackageArchive.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace Sora::Compute {

    /** @brief Failure while registering logical resources with device-loss
     * invalidation. */
    struct GraphResourceRegistrationFailure {
        RuntimeInvalidationError error{};
    };

    /** @brief Failure returned by the configured logical-to-physical planning
     * engine. */
    template<typename Error>
    struct GraphPlanningFailure {
        Error error;
    };

    /** @brief Transactional physical graph materialization failure. */
    struct GraphMaterializationFailure {
        PhysicalGraphMaterializationError error{};
    };

    /** @brief Pure planner output failed catalog validation or device-lease
     * capture. */
    struct GraphPlanValidationFailure {
        PhysicalGraphPlanError error{};
    };

    /** @brief Immutable planning inputs could not be frozen before invoking the
     * planner. */
    struct GraphPlanningInputFailure {
        ArtifactCacheError error = ArtifactCacheError::OutOfMemory;
    };

    /** @brief The immutable planning program catalog could not be constructed. */
    struct GraphProgramCatalogFailure {
        ProgramCatalogError error{};
    };

    /** @brief Exact preparation stages that may fail before physical submission
     * starts. */
    template<typename PlanningError>
    using GraphPreparationError =
        std::variant<GraphResourceRegistrationFailure, GraphPlanningInputFailure, GraphProgramCatalogFailure,
                     GraphPlanningFailure<PlanningError>, GraphPlanValidationFailure, GraphMaterializationFailure>;

    /** @brief Failure policy for an explicitly configured artifact store. */
    enum class ArtifactStoreInitializationPolicy : uint8_t {
        Required,
        Opportunistic,
    };

    /** @brief Construction policy owned by one isolated compute runtime context. */
    struct ComputeContextConfig {
        size_t artifactCacheCapacity = 256;
        std::optional<ArtifactStoreConfig> artifactStore{};
        ArtifactStoreInitializationPolicy artifactStoreInitialization = ArtifactStoreInitializationPolicy::Required;
        PortablePackageRegistryLimits packages{};
        PerformanceModelLimits performance{};
        PerformancePriorPolicy performancePriors{};
        GraphFusionPolicy fusion{};
        HostTransferCalibrationConfig hostTransfers{};
        GraphRecoveryPolicy recovery{};
    };

    /** @brief Stable failure while creating an owning compute runtime context. */
    enum class ComputeContextErrorCode : uint8_t {
        InvalidConfiguration,
        ArtifactStoreInitializationFailed,
        RuntimeInitializationFailed,
        InvalidationRegistrationFailed,
        HostTransferCalibrationFailed,
        OutOfMemory,
    };

    /** @brief Context construction failure retaining its precise nested runtime
     * cause. */
    struct ComputeContextError {
        ComputeContextErrorCode code = ComputeContextErrorCode::InvalidConfiguration;
        RuntimeRecoveryError recovery{};
        RuntimeInvalidationError invalidation{};
        HostTransferBridgeError hostTransfer{};
        ArtifactStoreError artifactStore{};

        friend constexpr bool operator==(ComputeContextError, ComputeContextError) noexcept = default;
    };

    /** @brief Archive validation/trust failure or transactional registry publication failure. */
    using PortablePackageArchiveRegistrationError =
        std::variant<PortablePackageArchiveError, PortablePackageRegistryError>;

    namespace Detail {

        [[nodiscard]] inline std::expected<void, RuntimeInvalidationError>
        RegisterBuffer(RuntimeInvalidationRegistry& registry, const Buffer& buffer) noexcept {
            auto registered = registry.Register(buffer);
            if (!registered) {
                return std::unexpected(registered.error());
            }
            return {};
        }

        [[nodiscard]] inline std::expected<void, RuntimeInvalidationError>
        RegisterOperationBuffers(RuntimeInvalidationRegistry& registry, const GraphOperation& operation) noexcept {
            return std::visit(
                [&registry]<typename Operation>(
                    const Operation& value) -> std::expected<void, RuntimeInvalidationError> {
                    if constexpr (std::same_as<Operation, KernelInvocation>) {
                        for (const KernelArgumentBinding& binding : value.Bindings()) {
                            if (const auto* view = std::get_if<BufferView>(&binding)) {
                                if (auto registered = RegisterBuffer(registry, view->LogicalBuffer()); !registered) {
                                    return registered;
                                }
                            }
                        }
                    } else if constexpr (std::same_as<Operation, CopyOperation>) {
                        if (auto registered = RegisterBuffer(registry, value.Source().LogicalBuffer()); !registered) {
                            return registered;
                        }
                        if (auto registered = RegisterBuffer(registry, value.Destination().LogicalBuffer());
                            !registered) {
                            return registered;
                        }
                    } else if constexpr (std::same_as<Operation, FillOperation>) {
                        if (auto registered = RegisterBuffer(registry, value.Destination().LogicalBuffer());
                            !registered) {
                            return registered;
                        }
                    } else {
                        for (const ExternalResourceBinding& binding : value.Resources()) {
                            if (auto registered = RegisterBuffer(registry, binding.view.LogicalBuffer()); !registered) {
                                return registered;
                            }
                        }
                    }
                    return {};
                },
                operation);
        }

        [[nodiscard]] inline std::expected<void, RuntimeInvalidationError>
        RegisterGraphBuffers(RuntimeInvalidationRegistry& registry, const ExecutionGraph& graph) noexcept {
            for (size_t index = 0; index < graph.NodeCount(); ++index) {
                if (auto registered =
                        RegisterOperationBuffers(registry, graph.Operation(GraphNodeId{static_cast<uint32_t>(index)}));
                    !registered) {
                    return registered;
                }
            }
            return {};
        }

        [[nodiscard]] inline std::expected<void, RuntimeInvalidationError>
        RegisterPlanBuffers(RuntimeInvalidationRegistry& registry, const PhysicalGraphPlan& plan) noexcept {
            for (const PlannedAllocation& allocation : plan.Allocations()) {
                if (auto registered = RegisterBuffer(registry, allocation.buffer); !registered) {
                    return registered;
                }
            }
            return {};
        }

    } // namespace Detail

    /**
     * @brief Owning, shareable runtime context binding planning, compilation,
     * recovery, and submission services.
     * @details Senders retain the shared context state and frozen logical graph, so
     * destroying the facade does not invalidate an operation. Planner invocations
     * may run concurrently and therefore must be thread-safe.
     */
    template<GraphPlanningEngine Planner, RuntimeEpochRecoveryProvider RecoveryProvider>
        requires std::is_nothrow_move_constructible_v<Planner> && std::is_nothrow_move_constructible_v<RecoveryProvider>
    class ComputeContext final {
        using PlanResult =
            std::invoke_result_t<Planner&, const ExecutionGraph&, const GraphPlanningContext&, GraphPlanningServices>;

        struct OptimizationTelemetry final {
            void Observe(GraphOptimizationReport report) noexcept {
                attempts.fetch_add(1, std::memory_order_relaxed);
                Counter(report.status).fetch_add(1, std::memory_order_relaxed);
            }

            [[nodiscard]] GraphOptimizationStatistics Snapshot() const noexcept {
                return {.attempts = attempts.load(std::memory_order_relaxed),
                        .unchanged = unchanged.load(std::memory_order_relaxed),
                        .disabled = disabled.load(std::memory_order_relaxed),
                        .fused = fused.load(std::memory_order_relaxed),
                        .skippedUnprofitable = skippedUnprofitable.load(std::memory_order_relaxed),
                        .skippedBudget = skippedBudget.load(std::memory_order_relaxed),
                        .failed = failed.load(std::memory_order_relaxed)};
            }

        private:
            [[nodiscard]] std::atomic<uint64_t>& Counter(GraphOptimizationStatus status) noexcept {
                switch (status) {
                    case GraphOptimizationStatus::Unchanged:
                        return unchanged;
                    case GraphOptimizationStatus::Disabled:
                        return disabled;
                    case GraphOptimizationStatus::Fused:
                        return fused;
                    case GraphOptimizationStatus::SkippedUnprofitable:
                        return skippedUnprofitable;
                    case GraphOptimizationStatus::SkippedBudget:
                        return skippedBudget;
                    case GraphOptimizationStatus::Failed:
                        return failed;
                }
                return failed;
            }

            std::atomic<uint64_t> attempts = 0;
            std::atomic<uint64_t> unchanged = 0;
            std::atomic<uint64_t> disabled = 0;
            std::atomic<uint64_t> fused = 0;
            std::atomic<uint64_t> skippedUnprofitable = 0;
            std::atomic<uint64_t> skippedBudget = 0;
            std::atomic<uint64_t> failed = 0;
        };

        struct State final {
            State(size_t cacheCapacity, PortablePackageRegistryLimits packageLimits,
                  PerformanceModelLimits performanceLimits, PerformancePriorPolicy priorPolicy,
                  GraphFusionPolicy fusionPolicy, GraphRecoveryPolicy policy,
                  HostTransferCalibrationConfig hostTransferPolicy, std::optional<ArtifactStore> persistentArtifacts,
                  std::unique_ptr<RuntimeRecoveryCoordinator> runtime, Planner graphPlanner,
                  RecoveryProvider epochProvider)
                : packages(packageLimits),
                  artifacts(cacheCapacity),
                  artifactStore(std::move(persistentArtifacts)),
                  performance(performanceLimits),
                  priors(priorPolicy),
                  compiler(artifacts, artifactStore ? std::addressof(*artifactStore) : nullptr, packages,
                           performance.ResolutionObserver()),
                  pipeline(artifacts, artifactStore ? std::addressof(*artifactStore) : nullptr, packages,
                           performance.ResolutionObserver()),
                  coordinator(std::move(runtime)),
                  planner(std::move(graphPlanner)),
                  rebuildEpoch(std::move(epochProvider)),
                  fusion(fusionPolicy),
                  hostTransfers(hostTransferPolicy),
                  defaultRecovery(policy) {}

            PortablePackageRegistry packages;
            ArtifactCache artifacts;
            std::optional<ArtifactStore> artifactStore;
            PerformanceModel performance;
            PerformancePriorModel priors;
            CompilerService compiler;
            PipelineService pipeline;
            std::unique_ptr<RuntimeRecoveryCoordinator> coordinator;
            Planner planner;
            RecoveryProvider rebuildEpoch;
            GraphFusionPolicy fusion{};
            OptimizationTelemetry optimization{};
            HostTransferCalibrationConfig hostTransfers{};
            GraphRecoveryPolicy defaultRecovery{};
        };

    public:
        using PlanningError = typename PlanResult::error_type;
        using PreparationError = GraphPreparationError<PlanningError>;
        using Error = RecoveringGraphError<PreparationError>;

        /** @brief Construct an isolated context and register its live-artifact cache
         * for device-loss invalidation. */
        [[nodiscard]] static std::expected<ComputeContext, ComputeContextError>
        Create(RuntimeEpoch initial, ComputeContextConfig config, Planner planner,
               RecoveryProvider recoveryProvider) noexcept {
            const bool validArtifactStorePolicy =
                config.artifactStoreInitialization == ArtifactStoreInitializationPolicy::Required ||
                config.artifactStoreInitialization == ArtifactStoreInitializationPolicy::Opportunistic;
            if (!config.packages.Valid() || !config.performance.Valid() || !config.performancePriors.Valid() ||
                !config.fusion.Valid() || !config.hostTransfers.Valid() || !config.recovery.Valid() ||
                !validArtifactStorePolicy) {
                return std::unexpected(ComputeContextError{ComputeContextErrorCode::InvalidConfiguration});
            }
            auto coordinator = RuntimeRecoveryCoordinator::Create(std::move(initial));
            if (!coordinator) {
                return std::unexpected(ComputeContextError{.code = ComputeContextErrorCode::RuntimeInitializationFailed,
                                                           .recovery = coordinator.error()});
            }
            std::optional<ArtifactStore> artifactStore;
            if (config.artifactStore) {
                auto created = ArtifactStore::Create(std::move(*config.artifactStore));
                if (!created) {
                    const bool mayFallback =
                        config.artifactStoreInitialization == ArtifactStoreInitializationPolicy::Opportunistic &&
                        created.error().code == ArtifactStoreErrorCode::IoError;
                    if (!mayFallback) {
                        return std::unexpected(ComputeContextError{
                            .code = ComputeContextErrorCode::ArtifactStoreInitializationFailed,
                            .artifactStore = created.error(),
                        });
                    }
                } else {
                    artifactStore.emplace(std::move(*created));
                }
            }
            try {
                auto state = std::make_shared<State>(
                    config.artifactCacheCapacity, config.packages, config.performance, config.performancePriors,
                    config.fusion, config.recovery, config.hostTransfers, std::move(artifactStore),
                    std::move(*coordinator), std::move(planner), std::move(recoveryProvider));
                const auto current = state->coordinator->Current();
                auto calibrated =
                    CalibrateHostTransferBridges(current->Resources(), state->performance, state->hostTransfers);
                if (!calibrated) {
                    return std::unexpected(
                        ComputeContextError{.code = ComputeContextErrorCode::HostTransferCalibrationFailed,
                                            .hostTransfer = calibrated.error()});
                }
                auto registered = state->coordinator->InvalidationRegistry().Register(state->artifacts);
                if (!registered) {
                    return std::unexpected(
                        ComputeContextError{.code = ComputeContextErrorCode::InvalidationRegistrationFailed,
                                            .invalidation = registered.error()});
                }
                return ComputeContext{std::move(state)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(ComputeContextError{ComputeContextErrorCode::OutOfMemory});
            }
        }

        /** @brief Register one canonical portable program for resolution by future
         * planning attempts. */
        template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError> RegisterProgram(
            const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>& program) noexcept {
            return state_->packages.Register(program);
        }

        /** @brief Atomically register one validated portable package with optional
         * exact-target artifact seeds. */
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageRegistryError>
        RegisterPackage(PortablePackage package) noexcept {
            return state_->packages.Register(std::move(package));
        }

        /** @brief Validate and register an archive trusted by an enclosing transport or local policy. */
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageArchiveRegistrationError>
        RegisterTrustedPackageArchive(std::span<const std::byte> bytes,
                                      PortablePackageArchiveLimits limits = {}) noexcept {
            auto archive = PortablePackageArchiveView::OpenTrusted(bytes, limits);
            if (!archive) {
                return std::unexpected(PortablePackageArchiveRegistrationError{
                    std::in_place_type<PortablePackageArchiveError>, archive.error()});
            }
            return RegisterPackageArchive(*archive, PortablePackageProvenance::TrustedArchive());
        }

        /** @brief Verify detached trust evidence, validate, and transactionally register one archive. */
        template<PortablePackageSignatureVerifier Verifier>
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageArchiveRegistrationError>
        RegisterVerifiedPackageArchive(std::span<const std::byte> bytes, PortablePackageDetachedSignature signature,
                                       const Verifier& verifier, PortablePackageArchiveLimits limits = {}) noexcept {
            auto archive = PortablePackageArchiveView::OpenVerified(bytes, signature, verifier, limits);
            if (!archive) {
                return std::unexpected(PortablePackageArchiveRegistrationError{
                    std::in_place_type<PortablePackageArchiveError>, archive.error()});
            }
            const auto provenance = archive->VerifiedProvenance();
            if (!provenance) {
                return std::unexpected(PortablePackageArchiveRegistrationError{
                    std::in_place_type<PortablePackageArchiveError>,
                    PortablePackageArchiveError{.code = PortablePackageArchiveErrorCode::SignatureRejected,
                                                .program = archive->ProgramIdentity()}});
            }
            return RegisterPackageArchive(*archive, *provenance);
        }

        /** @brief Access the authoritative portable-package registry for explicit
         * lifecycle management. */
        [[nodiscard]] PortablePackageRegistry& Packages() noexcept { return state_->packages; }

        /** @brief Access the authoritative portable-package registry without
         * permitting lifecycle changes. */
        [[nodiscard]] const PortablePackageRegistry& Packages() const noexcept { return state_->packages; }

        /** @brief Atomically revoke matching detached package admissions from the current registry generation. */
        [[nodiscard]] std::expected<PortablePackageAdmissionRevocationResult, PortablePackageRegistryError>
        RevokePackageAdmissions(PortablePackageAdmissionRevocation revocation) noexcept {
            return state_->packages.RevokeAdmissions(revocation);
        }

        /** @brief Access the context-wide bounded calibration service for explicit
         * backend observations. */
        [[nodiscard]] PerformanceModel& Performance() noexcept { return state_->performance; }

        /** @brief Access the context-wide bounded calibration service without
         * publishing observations. */
        [[nodiscard]] const PerformanceModel& Performance() const noexcept { return state_->performance; }

        /** @brief Observe the recovery coordinator for diagnostics and explicit
         * invalidation registration. */
        [[nodiscard]] RuntimeRecoveryCoordinator& Recovery() noexcept { return *state_->coordinator; }

        /** @brief Return the context-wide default recovery policy. */
        [[nodiscard]] GraphRecoveryPolicy DefaultRecoveryPolicy() const noexcept { return state_->defaultRecovery; }

        /** @brief Sample monotonic submission-scoped graph-optimization outcomes. */
        [[nodiscard]] GraphOptimizationStatistics OptimizationStatistics() const noexcept {
            return state_->optimization.Snapshot();
        }

        /** @brief Return a lazy move-only sender for one frozen logical graph with the default policy. */
        template<stdexec::scheduler Scheduler>
        [[nodiscard]] auto Submit(Scheduler scheduler, ExecutionGraph graph) const {
            return Submit(std::move(scheduler), std::move(graph), state_->defaultRecovery);
        }

        /** @brief Return a lazy move-only sender for one frozen logical graph with @p policy. */
        template<stdexec::scheduler Scheduler>
        [[nodiscard]] auto Submit(Scheduler scheduler, ExecutionGraph graph, GraphRecoveryPolicy policy) const {
            struct GraphPreparationState final {
                explicit GraphPreparationState(ExecutionGraph source) noexcept : source(std::move(source)) {}

                ExecutionGraph source;
                std::optional<GraphOptimizationResult> prepared{};
            };

            auto state = state_;
            auto preparation = std::make_shared<GraphPreparationState>(std::move(graph));
            auto factory =
                [state, preparation](
                    const GraphAttemptContext& attempt) noexcept -> std::expected<ExecutableGraph, PreparationError> {
                if (!preparation->prepared.has_value()) {
                    auto programs = ProgramCatalogSnapshot::Create(
                        state->packages.Snapshot(), {.maximumGeneratedPrograms = state->fusion.maximumFusionRegions,
                                                     .maximumGeneratedBytes = state->fusion.maximumGeneratedBytes});
                    if (!programs) {
                        return std::unexpected(PreparationError{GraphProgramCatalogFailure{programs.error()}});
                    }
                    auto optimized =
                        OptimizeExecutionGraph(std::move(preparation->source), std::move(*programs), state->fusion);
                    state->optimization.Observe(optimized.report);
                    preparation->prepared.emplace(std::move(optimized));
                }
                const ExecutionGraph& preparedGraph = preparation->prepared->graph;
                RuntimeInvalidationRegistry& invalidation = state->coordinator->InvalidationRegistry();
                if (auto registered = Detail::RegisterGraphBuffers(invalidation, preparedGraph); !registered) {
                    return std::unexpected(PreparationError{GraphResourceRegistrationFailure{registered.error()}});
                }

                const GraphPlanningContext planningContext{.epochSequence = attempt.epoch->Sequence(),
                                                           .devices = attempt.devices,
                                                           .snapshots = attempt.snapshots,
                                                           .previousSubmission = attempt.previousSubmission,
                                                           .attempt = attempt.attempt};
                auto artifacts = state->artifacts.SnapshotAvailability();
                if (!artifacts) {
                    return std::unexpected(PreparationError{GraphPlanningInputFailure{artifacts.error()}});
                }
                const auto performance = state->performance.Snapshot();
                auto planned =
                    std::invoke(state->planner, preparedGraph, planningContext,
                                GraphPlanningServices{
                                    preparation->prepared->programs, attempt.epoch->Resources().Inventory(), *artifacts,
                                    performance ? *performance : PerformanceSnapshot::Empty(), &state->priors});
                if (!planned) {
                    return std::unexpected(
                        PreparationError{GraphPlanningFailure<PlanningError>{std::move(planned.error())}});
                }
                auto plan = PhysicalGraphPlan::Create(std::move(planned->storage), attempt.epoch->Resources());
                if (!plan) {
                    return std::unexpected(PreparationError{GraphPlanValidationFailure{plan.error()}});
                }
                if (auto registered = Detail::RegisterPlanBuffers(invalidation, *plan); !registered) {
                    return std::unexpected(PreparationError{GraphResourceRegistrationFailure{registered.error()}});
                }

                auto materialized = MaterializePhysicalGraph(
                    *plan, attempt.epoch->Resources(),
                    ArtifactResolutionServices{state->compiler, state->pipeline, state->performance.KernelObserver()});
                if (!materialized) {
                    return std::unexpected(PreparationError{GraphMaterializationFailure{materialized.error()}});
                }
                return std::move(*materialized);
            };
            auto recoveryProvider = [state](const RuntimeRecoveryRequest& request) noexcept {
                auto replacement = std::invoke(state->rebuildEpoch, request);
                if (!replacement) {
                    return replacement;
                }
                auto calibrated =
                    CalibrateHostTransferBridges(replacement->Resources(), state->performance, state->hostTransfers);
                if (!calibrated) {
                    return std::expected<RuntimeEpoch, RuntimeRecoveryError>{std::unexpected(
                        RuntimeRecoveryError{.code = RuntimeRecoveryErrorCode::HostTransferCalibrationFailed,
                                             .service = request.failedService,
                                             .expectedSequence = request.sequence,
                                             .actualSequence = replacement->Sequence(),
                                             .hostTransfer = calibrated.error()})};
                }
                return replacement;
            };
            return RecoveringSubmitOn(std::move(scheduler), *state->coordinator, policy, std::move(factory),
                                      std::move(recoveryProvider));
        }

    private:
        [[nodiscard]] std::expected<PortablePackageLease, PortablePackageArchiveRegistrationError>
        RegisterPackageArchive(const PortablePackageArchiveView& archive,
                               PortablePackageProvenance provenance) noexcept {
            auto package = archive.Materialize();
            if (!package) {
                return std::unexpected(PortablePackageArchiveRegistrationError{
                    std::in_place_type<PortablePackageArchiveError>, package.error()});
            }
            auto registered = state_->packages.Register(std::move(*package), provenance);
            if (!registered) {
                return std::unexpected(PortablePackageArchiveRegistrationError{
                    std::in_place_type<PortablePackageRegistryError>, registered.error()});
            }
            return std::move(*registered);
        }

        explicit ComputeContext(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    /** @brief Deduce planner and recovery-provider types while constructing one
     * owning compute context. */
    template<GraphPlanningEngine Planner, RuntimeEpochRecoveryProvider RecoveryProvider>
    [[nodiscard]] auto CreateComputeContext(RuntimeEpoch initial, ComputeContextConfig config, Planner planner,
                                            RecoveryProvider recoveryProvider) noexcept
        -> std::expected<ComputeContext<Planner, RecoveryProvider>, ComputeContextError> {
        return ComputeContext<Planner, RecoveryProvider>::Create(std::move(initial), config, std::move(planner),
                                                                 std::move(recoveryProvider));
    }

    static_assert(std::is_nothrow_move_constructible_v<ExecutionGraph>);

} // namespace Sora::Compute
