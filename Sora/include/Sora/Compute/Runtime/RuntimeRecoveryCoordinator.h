/**
 * @file RuntimeRecoveryCoordinator.h
 * @brief Single-flight reconstruction and atomic publication of runtime epochs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/HostTransferBridge.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>
#include <Sora/Compute/Runtime/RuntimeInvalidationRegistry.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace Sora::Compute {

    /** @brief Stable failure while coordinating one physical-runtime reconstruction. */
    enum class RuntimeRecoveryErrorCode : uint8_t {
        InvalidObservation,
        UnsupportedObservationFailure,
        SequenceExhausted,
        InvalidationFailed,
        EpochRebuildFailed,
        RuntimeDiscoveryFailed,
        HostTransferCalibrationFailed,
        EpochDiffFailed,
        ReplacementSequenceMismatch,
        FailedServiceRetained,
        ConcurrentPublication,
        OutOfMemory,
    };

    /** @brief Recovery failure preserving service, sequence, observation, and invalidation identity. */
    struct RuntimeRecoveryError {
        RuntimeRecoveryErrorCode code = RuntimeRecoveryErrorCode::InvalidObservation;
        BackendServiceId service{};
        uint64_t expectedSequence = 0;
        uint64_t actualSequence = 0;
        RuntimeEpochError observation{};
        RuntimeDiscoveryError discovery{};
        RuntimeEpochDiffError epochDiff{};
        RuntimeInvalidationError invalidation{};
        HostTransferBridgeError hostTransfer{};

        friend constexpr bool operator==(RuntimeRecoveryError, RuntimeRecoveryError) noexcept = default;
    };

    /** @brief Immutable input for one runtime-epoch reconstruction attempt. */
    struct RuntimeRecoveryRequest {
        std::shared_ptr<const RuntimeEpoch> previous{};
        BackendServiceId failedService{};
        uint64_t sequence = 0;
    };

    /** @brief A noexcept epoch factory used only by the elected single-flight leader. */
    template<typename Provider>
    concept RuntimeEpochRecoveryProvider = requires(Provider& provider, const RuntimeRecoveryRequest& request) {
        { std::invoke(provider, request) } noexcept -> std::same_as<std::expected<RuntimeEpoch, RuntimeRecoveryError>>;
    };

    /**
     * @brief Process-local coordinator serializing invalidation, reconstruction, and epoch publication.
     * @details At most one provider executes at a time. Waiters for the same failed service share its exact result;
     * waiters for another failed service re-evaluate the published replacement and rebuild only if still necessary.
     */
    class RuntimeRecoveryCoordinator final {
        struct FlightState {
            explicit FlightState(BackendServiceId value) noexcept : service(value) {}

            BackendServiceId service{};
            bool completed = false;
            std::optional<RuntimeRecoveryError> error{};
        };

    public:
        RuntimeRecoveryCoordinator(const RuntimeRecoveryCoordinator&) = delete;
        RuntimeRecoveryCoordinator& operator=(const RuntimeRecoveryCoordinator&) = delete;

        /** @brief Create a coordinator and publish @p initial as its first immutable epoch. */
        [[nodiscard]] static std::expected<std::unique_ptr<RuntimeRecoveryCoordinator>, RuntimeRecoveryError>
        Create(RuntimeEpoch initial) noexcept;

        /** @brief Acquire the currently published immutable epoch. */
        [[nodiscard]] std::shared_ptr<const RuntimeEpoch> Current() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
            return current_.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
        }

        /** @brief Access the process-local cache and buffer invalidation registry. */
        [[nodiscard]] RuntimeInvalidationRegistry& InvalidationRegistry() noexcept { return invalidation_; }

        /**
         * @brief Recover from a canonical observation failure and return an epoch excluding its failed service.
         * @details @p provider is invoked only by a single-flight leader and must construct exactly request.sequence.
         */
        template<RuntimeEpochRecoveryProvider Provider>
        [[nodiscard]] std::expected<std::shared_ptr<const RuntimeEpoch>, RuntimeRecoveryError>
        Recover(const std::shared_ptr<const RuntimeEpoch>& observed, RuntimeEpochError failure,
                Provider&& provider) noexcept {
            if (!observed || failure.device >= observed->Devices().size()) {
                return std::unexpected(Failure(RuntimeRecoveryErrorCode::InvalidObservation, {}, 0, 0, failure));
            }
            if (!Recoverable(failure.code)) {
                return std::unexpected(
                    Failure(RuntimeRecoveryErrorCode::UnsupportedObservationFailure, {}, 0, 0, failure));
            }
            return RecoverService(observed, observed->Devices()[failure.device].service,
                                  std::forward<Provider>(provider));
        }

    private:
        explicit RuntimeRecoveryCoordinator(std::shared_ptr<const RuntimeEpoch> initial) noexcept
            : current_(std::move(initial)) {}

        void PublishCurrent(std::shared_ptr<const RuntimeEpoch> epoch) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
            current_.store(std::move(epoch), std::memory_order_release);
#else
            std::atomic_store_explicit(&current_, std::move(epoch), std::memory_order_release);
#endif
        }

        [[nodiscard]] static constexpr bool Recoverable(RuntimeEpochErrorCode code) noexcept {
            return code == RuntimeEpochErrorCode::DeviceLost || code == RuntimeEpochErrorCode::DeviceGenerationChanged;
        }

        [[nodiscard]] static bool ContainsService(const RuntimeEpoch& epoch, BackendServiceId service) noexcept {
            return std::ranges::any_of(
                epoch.Devices(), [service](const RuntimeDeviceBinding& device) { return device.service == service; });
        }

        [[nodiscard]] static constexpr RuntimeRecoveryError
        Failure(RuntimeRecoveryErrorCode code, BackendServiceId service = {}, uint64_t expectedSequence = 0,
                uint64_t actualSequence = 0, RuntimeEpochError observation = {},
                RuntimeInvalidationError invalidation = {}, RuntimeEpochDiffError epochDiff = {}) noexcept {
            return {.code = code,
                    .service = service,
                    .expectedSequence = expectedSequence,
                    .actualSequence = actualSequence,
                    .observation = observation,
                    .epochDiff = epochDiff,
                    .invalidation = invalidation};
        }

        void CompleteFlight(const std::shared_ptr<FlightState>& flight,
                            std::optional<RuntimeRecoveryError> error) noexcept {
            {
                std::scoped_lock lock{mutex_};
                flight->error = std::move(error);
                flight->completed = true;
                if (activeFlight_ == flight) {
                    activeFlight_.reset();
                }
            }
            completed_.notify_all();
        }

        template<RuntimeEpochRecoveryProvider Provider>
        [[nodiscard]] std::expected<std::shared_ptr<const RuntimeEpoch>, RuntimeRecoveryError>
        RecoverService(std::shared_ptr<const RuntimeEpoch> base, BackendServiceId failedService,
                       Provider&& provider) noexcept {
            while (true) {
                std::shared_ptr<const RuntimeEpoch> current = Current();
                if (current != base) {
                    if (!current || current->Sequence() <= base->Sequence()) {
                        return std::unexpected(Failure(RuntimeRecoveryErrorCode::InvalidObservation, failedService));
                    }
                    base = std::move(current);
                }
                if (!ContainsService(*base, failedService)) {
                    return base;
                }
                if (base->Sequence() == std::numeric_limits<uint64_t>::max()) {
                    return std::unexpected(Failure(RuntimeRecoveryErrorCode::SequenceExhausted, failedService));
                }

                std::shared_ptr<FlightState> flight;
                {
                    std::unique_lock lock{mutex_};
                    current = Current();
                    if (current != base) {
                        continue;
                    }
                    if (activeFlight_) {
                        flight = activeFlight_;
                        completed_.wait(lock, [&flight] { return flight->completed; });
                        if (flight->service == failedService && flight->error) {
                            return std::unexpected(*flight->error);
                        }
                        continue;
                    }
                    try {
                        flight = std::make_shared<FlightState>(failedService);
                    } catch (const std::bad_alloc&) {
                        return std::unexpected(Failure(RuntimeRecoveryErrorCode::OutOfMemory, failedService));
                    }
                    activeFlight_ = flight;
                }

                const uint64_t nextSequence = base->Sequence() + 1;
                auto invalidated = invalidation_.InvalidateService(failedService);
                if (!invalidated) {
                    const RuntimeRecoveryError error = Failure(RuntimeRecoveryErrorCode::InvalidationFailed,
                                                               failedService, nextSequence, 0, {}, invalidated.error());
                    CompleteFlight(flight, error);
                    return std::unexpected(error);
                }

                RuntimeRecoveryRequest request{
                    .previous = base,
                    .failedService = failedService,
                    .sequence = nextSequence,
                };
                auto replacement = std::invoke(provider, request);
                if (!replacement) {
                    CompleteFlight(flight, replacement.error());
                    return std::unexpected(replacement.error());
                }
                if (replacement->Sequence() != nextSequence) {
                    const RuntimeRecoveryError error = Failure(RuntimeRecoveryErrorCode::ReplacementSequenceMismatch,
                                                               failedService, nextSequence, replacement->Sequence());
                    CompleteFlight(flight, error);
                    return std::unexpected(error);
                }
                if (ContainsService(*replacement, failedService)) {
                    const RuntimeRecoveryError error = Failure(RuntimeRecoveryErrorCode::FailedServiceRetained,
                                                               failedService, nextSequence, replacement->Sequence());
                    CompleteFlight(flight, error);
                    return std::unexpected(error);
                }

                auto diff = DiffRuntimeEpochs(*base, *replacement);
                if (!diff) {
                    const RuntimeRecoveryError error =
                        Failure(RuntimeRecoveryErrorCode::EpochDiffFailed, diff.error().service, nextSequence,
                                replacement->Sequence(), {}, {}, diff.error());
                    CompleteFlight(flight, error);
                    return std::unexpected(error);
                }

                std::shared_ptr<const RuntimeEpoch> published;
                try {
                    published = std::make_shared<const RuntimeEpoch>(std::move(*replacement));
                } catch (const std::bad_alloc&) {
                    const RuntimeRecoveryError error =
                        Failure(RuntimeRecoveryErrorCode::OutOfMemory, failedService, nextSequence);
                    CompleteFlight(flight, error);
                    return std::unexpected(error);
                }
                for (BackendServiceId service : diff->removedServices) {
                    if (service == failedService) {
                        continue;
                    }
                    auto retired = invalidation_.InvalidateService(service);
                    if (!retired) {
                        const RuntimeRecoveryError error =
                            Failure(RuntimeRecoveryErrorCode::InvalidationFailed, service, nextSequence,
                                    replacement->Sequence(), {}, retired.error());
                        CompleteFlight(flight, error);
                        return std::unexpected(error);
                    }
                }
                {
                    std::scoped_lock lock{mutex_};
                    if (Current() != base || activeFlight_ != flight) {
                        const RuntimeRecoveryError error =
                            Failure(RuntimeRecoveryErrorCode::ConcurrentPublication, failedService, nextSequence);
                        flight->error = error;
                        flight->completed = true;
                        if (activeFlight_ == flight) {
                            activeFlight_.reset();
                        }
                        completed_.notify_all();
                        return std::unexpected(error);
                    }
                    PublishCurrent(published);
                    flight->completed = true;
                    activeFlight_.reset();
                }
                completed_.notify_all();
                return published;
            }
        }

#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<const RuntimeEpoch>> current_;
#else
        std::shared_ptr<const RuntimeEpoch> current_;
#endif
        RuntimeInvalidationRegistry invalidation_;
        mutable std::mutex mutex_;
        std::condition_variable completed_;
        std::shared_ptr<FlightState> activeFlight_{};
    };

} // namespace Sora::Compute
