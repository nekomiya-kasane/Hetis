/**
 * @file Queue.h
 * @brief Versioned physical-command submission ABI and shared queue ownership.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Compute/Backend/Event.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Memory/Allocation.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Fixed rank capacity of the versioned backend dispatch-command ABI. */
    inline constexpr uint8_t kBackendCommandMaximumDispatchRank = 3;

    /** @brief Physical command understood by a backend queue callback. */
    enum class BackendCommandKind : uint8_t {
        Dispatch,
        Copy,
        Fill,
    };

    /** @brief Stable identity of one backend queue within a physical device. */
    struct QueueId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes one backend queue. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(QueueId, QueueId) noexcept = default;
        friend constexpr auto operator<=>(QueueId, QueueId) noexcept = default;
    };

    /** @brief Physical command families accepted by one backend queue. */
    enum class QueueCapability : uint32_t {
        Dispatch = uint32_t{1} << 0,
        Copy = uint32_t{1} << 1,
        Fill = uint32_t{1} << 2,
    };

    /** @brief Compact validated set of queue command capabilities. */
    class QueueCapabilityMask {
    public:
        constexpr QueueCapabilityMask() noexcept = default;
        constexpr QueueCapabilityMask(QueueCapability capability) noexcept : bits_(std::to_underlying(capability)) {}
        constexpr explicit QueueCapabilityMask(uint32_t bits) noexcept : bits_(bits) {}

        /** @brief Return whether the set is nonempty and contains no unknown capability bits. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            constexpr uint32_t known = std::to_underlying(QueueCapability::Dispatch) |
                                       std::to_underlying(QueueCapability::Copy) |
                                       std::to_underlying(QueueCapability::Fill);
            return bits_ != 0 && (bits_ & ~known) == 0;
        }

        /** @brief Return whether @p command belongs to a supported command family. */
        [[nodiscard]] constexpr bool Supports(BackendCommandKind command) const noexcept {
            switch (command) {
                case BackendCommandKind::Dispatch:
                    return (bits_ & std::to_underlying(QueueCapability::Dispatch)) != 0;
                case BackendCommandKind::Copy:
                    return (bits_ & std::to_underlying(QueueCapability::Copy)) != 0;
                case BackendCommandKind::Fill:
                    return (bits_ & std::to_underlying(QueueCapability::Fill)) != 0;
            }
            return false;
        }

        /** @brief Return the stable raw bit representation. */
        [[nodiscard]] constexpr uint32_t Bits() const noexcept { return bits_; }

        friend constexpr bool operator==(QueueCapabilityMask, QueueCapabilityMask) noexcept = default;

        friend constexpr QueueCapabilityMask operator|(QueueCapabilityMask left, QueueCapabilityMask right) noexcept {
            return QueueCapabilityMask(left.bits_ | right.bits_);
        }

    private:
        uint32_t bits_ = 0;
    };

    [[nodiscard]] constexpr QueueCapabilityMask operator|(QueueCapability left, QueueCapability right) noexcept {
        return QueueCapabilityMask(left) | QueueCapabilityMask(right);
    }

    /** @brief Interpretation of one physical buffer binding. */
    enum class BackendBindingKind : uint8_t {
        OpaqueBytes,
        AffineElements,
    };

    /** @brief One opaque or affine dispatch binding into a backend-owned physical allocation. */
    struct BackendBinding {
        AllocationView allocation{};
        uint64_t offset = 0;
        uint64_t size = 0;
        uint32_t binding = 0;
        AccessMode access = AccessMode::Read;
        BackendBindingKind kind = BackendBindingKind::OpaqueBytes;
        ValueType elementType{};
        LayoutKind layout = LayoutKind::Scalar;
        uint8_t rank = 0;
        uint64_t extents[kMaximumBufferRank]{};
        uint64_t byteStrides[kMaximumBufferRank]{};
        uint64_t componentStrideBytes = 0;
    };

    /** @brief One canonical scalar argument stored in the submission's immutable scalar byte arena. */
    struct BackendScalarBinding {
        uint64_t dataOffset = 0;
        uint32_t size = 0;
        uint32_t binding = 0;
        ValueType type{};
    };

    /** @brief One physical dispatch, copy, or fill command in a contiguous submission batch. */
    struct BackendCommand {
        BackendCommandKind kind = BackendCommandKind::Dispatch;
        uint32_t firstBinding = 0;
        uint32_t bindingCount = 0;
        uint32_t firstScalarBinding = 0;
        uint32_t scalarBindingCount = 0;
        ArtifactView artifact{};
        AllocationView source{};
        AllocationView target{};
        uint64_t sourceOffset = 0;
        uint64_t targetOffset = 0;
        uint64_t sizeBytes = 0;
        uint64_t dispatchOrigin[kBackendCommandMaximumDispatchRank]{};
        uint64_t dispatchExtents[kBackendCommandMaximumDispatchRank]{};
        uint64_t globalExtents[kBackendCommandMaximumDispatchRank]{};
        uint32_t workgroupCount[3] = {1, 1, 1};
        uint8_t fillPattern[16]{};
        uint8_t fillPatternSize = 0;
    };

    /** @brief Allocation-free ABI request for one ordered command batch. */
    struct BackendSubmitRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendSubmitRequest);
        const BackendCommand* commands = nullptr;
        uint32_t commandCount = 0;
        const BackendBinding* bindings = nullptr;
        uint32_t bindingCount = 0;
        const std::byte* scalarData = nullptr;
        uint64_t scalarDataSize = 0;
        const BackendScalarBinding* scalarBindings = nullptr;
        uint32_t scalarBindingCount = 0;
        const EventView* waits = nullptr;
        uint32_t waitCount = 0;

        /** @brief Return the command batch without copying. */
        [[nodiscard]] constexpr std::span<const BackendCommand> Commands() const noexcept {
            return {commands, commandCount};
        }

        /** @brief Return every binding referenced by commands. */
        [[nodiscard]] constexpr std::span<const BackendBinding> Bindings() const noexcept {
            return {bindings, bindingCount};
        }

        /** @brief Return every scalar binding referenced by commands. */
        [[nodiscard]] constexpr std::span<const BackendScalarBinding> ScalarBindings() const noexcept {
            return {scalarBindings, scalarBindingCount};
        }

        /** @brief Return backend events that must complete before this batch starts. */
        [[nodiscard]] constexpr std::span<const EventView> Waits() const noexcept { return {waits, waitCount}; }
    };

    /** @brief Runtime-allocated callback result; a non-null event payload denotes accepted asynchronous work. */
    struct BackendSubmitResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendSubmitResult);
        BackendEventPayload event{};
        SubmissionError error{SubmissionErrorCode::SubmissionFailed, 0};
    };

    using BackendSubmit = void (*)(void* state, const BackendSubmitRequest* request,
                                   BackendSubmitResult* result) noexcept;
    using BackendQueueDestroy = void (*)(void* state) noexcept;

    /** @brief ABI payload transferring ownership of one backend queue to the runtime. */
    struct BackendQueuePayload {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendQueuePayload);
        QueueId id{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        QueueCapabilityMask capabilities{};
        ArtifactKind dispatchArtifactKind = ArtifactKind::DeviceExecutable;
        void* state = nullptr;
        BackendQueueDestroy destroy = nullptr;
        BackendSubmit submit = nullptr;
        uint8_t reportsExecutionNanoseconds = 0;
    };

    /** @brief Non-owning queue identity used by planners and diagnostics. */
    struct QueueView {
        QueueId id{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        QueueCapabilityMask capabilities{};
        ArtifactKind dispatchArtifactKind = ArtifactKind::DeviceExecutable;
        bool reportsExecutionNanoseconds = false;
        void* state = nullptr;
        BackendServiceId service{};

        /** @brief Return whether this view denotes a backend queue. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return state != nullptr && service.Valid(); }

        friend constexpr bool operator==(QueueView, QueueView) noexcept = default;
    };

    /** @brief Failure while adopting a queue payload. */
    enum class QueueError : uint8_t {
        InvalidPayload,
        OutOfMemory,
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidBackendCommandKind(BackendCommandKind kind) noexcept {
            return kind == BackendCommandKind::Dispatch || kind == BackendCommandKind::Copy ||
                   kind == BackendCommandKind::Fill;
        }

        [[nodiscard]] constexpr bool ValidBackendBindingKind(BackendBindingKind kind) noexcept {
            return kind == BackendBindingKind::OpaqueBytes || kind == BackendBindingKind::AffineElements;
        }

        [[nodiscard]] constexpr bool CheckedBindingMultiply(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (left != 0 && right > UINT64_MAX / left) {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] constexpr bool CheckedBindingAdd(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (right > UINT64_MAX - left) {
                return false;
            }
            result = left + right;
            return true;
        }

        [[nodiscard]] constexpr bool ValidAffineBinding(const BackendBinding& binding) noexcept {
            if (!binding.elementType.Valid() || !ValidLayoutKind(binding.layout) ||
                binding.layout == LayoutKind::Scalar || binding.rank == 0 || binding.rank > kMaximumBufferRank ||
                binding.componentStrideBytes == 0) {
                return false;
            }
            uint64_t footprint = 0;
            for (uint8_t axis = 0; axis < kMaximumBufferRank; ++axis) {
                const bool active = axis < binding.rank;
                if ((active && (binding.extents[axis] == 0 || binding.byteStrides[axis] == 0)) ||
                    (!active && (binding.extents[axis] != 0 || binding.byteStrides[axis] != 0))) {
                    return false;
                }
                if (active) {
                    uint64_t contribution = 0;
                    if (!CheckedBindingMultiply(binding.extents[axis] - 1, binding.byteStrides[axis], contribution) ||
                        !CheckedBindingAdd(footprint, contribution, footprint)) {
                        return false;
                    }
                }
            }
            uint64_t componentOffset = 0;
            const uint64_t scalarBytes = ScalarByteSize(binding.elementType.scalar);
            return CheckedBindingMultiply(binding.elementType.lanes - 1, binding.componentStrideBytes,
                                          componentOffset) &&
                   CheckedBindingAdd(footprint, componentOffset, footprint) &&
                   CheckedBindingAdd(footprint, scalarBytes, footprint) && footprint == binding.size;
        }

        [[nodiscard]] constexpr bool ValidBackendAccessMode(AccessMode access) noexcept {
            return Sora::Compute::ValidAccessMode(access);
        }

        [[nodiscard]] constexpr bool ValidBinding(const BackendBinding& binding, BackendKind backend, DeviceId device,
                                                  BackendServiceId service) noexcept {
            return binding.allocation && binding.allocation.location.backend == backend &&
                   binding.allocation.location.device == device && binding.allocation.service == service &&
                   binding.size != 0 && binding.allocation.Covers(binding.offset, binding.size) &&
                   ValidBackendAccessMode(binding.access) && ValidBackendBindingKind(binding.kind) &&
                   (binding.kind == BackendBindingKind::OpaqueBytes || ValidAffineBinding(binding));
        }

        [[nodiscard]] constexpr bool ValidScalarBinding(const BackendScalarBinding& binding,
                                                        const BackendSubmitRequest& request) noexcept {
            if (!binding.type.Valid() || binding.size != binding.type.ByteSize() ||
                binding.dataOffset > request.scalarDataSize ||
                binding.size > request.scalarDataSize - binding.dataOffset) {
                return false;
            }
            if (binding.type.scalar == ScalarKind::Bool) {
                for (uint32_t index = 0; index < binding.size; ++index) {
                    const std::byte value = request.scalarData[binding.dataOffset + index];
                    if (value != std::byte{0} && value != std::byte{1}) {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] constexpr bool DistinctBindingIndices(const BackendCommand& command,
                                                            const BackendSubmitRequest& request) noexcept {
            for (uint32_t left = 0; left < command.bindingCount; ++left) {
                const uint32_t binding = request.bindings[command.firstBinding + left].binding;
                for (uint32_t right = left + 1; right < command.bindingCount; ++right) {
                    if (binding == request.bindings[command.firstBinding + right].binding) {
                        return false;
                    }
                }
                for (uint32_t scalar = 0; scalar < command.scalarBindingCount; ++scalar) {
                    if (binding == request.scalarBindings[command.firstScalarBinding + scalar].binding) {
                        return false;
                    }
                }
            }
            for (uint32_t left = 0; left < command.scalarBindingCount; ++left) {
                const uint32_t binding = request.scalarBindings[command.firstScalarBinding + left].binding;
                for (uint32_t right = left + 1; right < command.scalarBindingCount; ++right) {
                    if (binding == request.scalarBindings[command.firstScalarBinding + right].binding) {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] constexpr bool ValidCommand(const BackendCommand& command, BackendKind backend, DeviceId device,
                                                  BackendServiceId service, ArtifactKind dispatchArtifactKind,
                                                  const BackendSubmitRequest& request) noexcept {
            if (!ValidBackendCommandKind(command.kind) || command.firstBinding > request.bindingCount ||
                command.bindingCount > request.bindingCount - command.firstBinding ||
                command.firstScalarBinding > request.scalarBindingCount ||
                command.scalarBindingCount > request.scalarBindingCount - command.firstScalarBinding) {
                return false;
            }
            if (command.kind == BackendCommandKind::Dispatch) {
                const bool implicitWholeDomain = command.dispatchExtents[0] == 0;
                bool inactiveAxis = false;
                for (uint8_t axis = 0; axis < kBackendCommandMaximumDispatchRank; ++axis) {
                    if (implicitWholeDomain &&
                        (command.dispatchOrigin[axis] != 0 || command.dispatchExtents[axis] != 0 ||
                         command.globalExtents[axis] != 0)) {
                        return false;
                    }
                    if (!implicitWholeDomain) {
                        if (command.dispatchExtents[axis] == 0) {
                            inactiveAxis = true;
                            if (command.dispatchOrigin[axis] != 0 || command.globalExtents[axis] != 0 ||
                                command.workgroupCount[axis] != 1) {
                                return false;
                            }
                        } else if (inactiveAxis || command.globalExtents[axis] == 0 ||
                                   command.dispatchOrigin[axis] > command.globalExtents[axis] ||
                                   command.dispatchExtents[axis] >
                                       command.globalExtents[axis] - command.dispatchOrigin[axis]) {
                            return false;
                        }
                    }
                }
                return command.artifact && command.artifact.kind == dispatchArtifactKind &&
                       command.artifact.backend == backend && command.artifact.device == device &&
                       command.artifact.service == service && command.workgroupCount[0] != 0 &&
                       command.workgroupCount[1] != 0 && command.workgroupCount[2] != 0 &&
                       DistinctBindingIndices(command, request);
            }
            if (command.bindingCount != 0 || command.scalarBindingCount != 0 || command.sizeBytes == 0) {
                return false;
            }
            if (command.kind == BackendCommandKind::Copy) {
                const auto sourceRange = command.source.PhysicalRange(command.sourceOffset, command.sizeBytes);
                const auto targetRange = command.target.PhysicalRange(command.targetOffset, command.sizeBytes);
                return sourceRange && targetRange && !sourceRange->Intersects(*targetRange) &&
                       command.source.location.backend == backend && command.target.location.backend == backend &&
                       command.source.location.device == device && command.target.location.device == device &&
                       command.source.service == service && command.target.service == service &&
                       command.source.Covers(command.sourceOffset, command.sizeBytes) &&
                       command.target.Covers(command.targetOffset, command.sizeBytes);
            }
            const bool validPattern = command.fillPatternSize == 1 || command.fillPatternSize == 2 ||
                                      command.fillPatternSize == 4 || command.fillPatternSize == 8 ||
                                      command.fillPatternSize == 16;
            return command.target && validPattern && command.targetOffset % command.fillPatternSize == 0 &&
                   command.sizeBytes % command.fillPatternSize == 0 && command.target.location.backend == backend &&
                   command.target.location.device == device && command.target.service == service &&
                   command.target.Covers(command.targetOffset, command.sizeBytes);
        }

        [[nodiscard]] constexpr bool ValidSubmitRequest(const BackendSubmitRequest& request, BackendKind backend,
                                                        DeviceId device, BackendServiceId service,
                                                        QueueCapabilityMask capabilities,
                                                        ArtifactKind dispatchArtifactKind) noexcept {
            if (request.abiVersion != kComputeBackendAbiVersion || request.structSize < sizeof(BackendSubmitRequest) ||
                (request.commandCount != 0 && request.commands == nullptr) || !capabilities.Valid() ||
                (dispatchArtifactKind != ArtifactKind::DeviceExecutable &&
                 dispatchArtifactKind != ArtifactKind::DevicePipeline) ||
                (request.bindingCount != 0 && request.bindings == nullptr) ||
                request.scalarDataSize > std::numeric_limits<size_t>::max() ||
                (request.scalarDataSize != 0 && request.scalarData == nullptr) ||
                (request.scalarBindingCount != 0 && request.scalarBindings == nullptr) ||
                (request.waitCount != 0 && request.waits == nullptr)) {
                return false;
            }
            if (request.commandCount == 0 &&
                (request.bindingCount != 0 || request.scalarDataSize != 0 || request.scalarBindingCount != 0)) {
                return false;
            }
            for (const BackendBinding& binding : request.Bindings()) {
                if (!ValidBinding(binding, backend, device, service)) {
                    return false;
                }
            }
            for (const BackendScalarBinding& binding : request.ScalarBindings()) {
                if (!ValidScalarBinding(binding, request)) {
                    return false;
                }
            }
            for (const BackendCommand& command : request.Commands()) {
                if (!capabilities.Supports(command.kind) ||
                    !ValidCommand(command, backend, device, service, dispatchArtifactKind, request)) {
                    return false;
                }
            }
            for (const EventView& wait : request.Waits()) {
                if (!wait || wait.backend != backend || wait.device != device || wait.service != service) {
                    return false;
                }
            }
            return true;
        }

    } // namespace Detail

    /** @brief Shared RAII queue that validates and submits physical command batches once per graph segment. */
    class Queue {
        struct Control {
            Control(BackendQueuePayload value, BackendService serviceOwner) noexcept
                : payload(value), service(std::move(serviceOwner)) {}
            ~Control() noexcept { payload.destroy(payload.state); }

            BackendQueuePayload payload;
            BackendService service;
        };

    public:
        Queue() noexcept = default;

        /** @brief Adopt @p payload and arrange exactly-once queue destruction. */
        [[nodiscard]] static std::expected<Queue, QueueError> Adopt(BackendService service,
                                                                    BackendQueuePayload payload) noexcept {
            const bool valid = payload.abiVersion == kComputeBackendAbiVersion &&
                               payload.structSize >= sizeof(BackendQueuePayload) && payload.id.Valid() &&
                               payload.device.Valid() && ValidBackendKind(payload.backend) &&
                               payload.capabilities.Valid() &&
                               (payload.dispatchArtifactKind == ArtifactKind::DeviceExecutable ||
                                payload.dispatchArtifactKind == ArtifactKind::DevicePipeline) &&
                               payload.state != nullptr && payload.destroy != nullptr && payload.submit != nullptr &&
                               payload.reportsExecutionNanoseconds <= 1 && service;
            if (!valid) {
                if (payload.state != nullptr && payload.destroy != nullptr) {
                    payload.destroy(payload.state);
                }
                return std::unexpected(QueueError::InvalidPayload);
            }
            try {
                return Queue{std::make_shared<Control>(payload, std::move(service))};
            } catch (const std::bad_alloc&) {
                payload.destroy(payload.state);
                return std::unexpected(QueueError::OutOfMemory);
            }
        }

        /** @brief Return whether this handle owns a backend queue. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return the non-owning queue identity. */
        [[nodiscard]] QueueView View() const noexcept {
            return control_ != nullptr
                       ? QueueView{.id = control_->payload.id,
                                   .device = control_->payload.device,
                                   .backend = control_->payload.backend,
                                   .capabilities = control_->payload.capabilities,
                                   .dispatchArtifactKind = control_->payload.dispatchArtifactKind,
                                   .reportsExecutionNanoseconds = control_->payload.reportsExecutionNanoseconds != 0,
                                   .state = control_->payload.state,
                                   .service = control_->service.Id()}
                       : QueueView{};
        }

        /**
         * @brief Validate and submit @p request, adopting the returned completion event transactionally.
         * @pre Artifact and allocation owners referenced by @p request outlive the returned event.
         */
        [[nodiscard]] std::expected<Event, SubmissionError> Submit(const BackendSubmitRequest& request) const noexcept {
            if (control_ == nullptr ||
                !Detail::ValidSubmitRequest(request, control_->payload.backend, control_->payload.device,
                                            control_->service.Id(), control_->payload.capabilities,
                                            control_->payload.dispatchArtifactKind)) {
                return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
            }

            BackendSubmitResult result{};
            control_->payload.submit(control_->payload.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendSubmitResult)) ||
                !Detail::ValidBackendSubmissionErrorCode(result.error.code)) {
                if (result.event.state != nullptr && result.event.destroy != nullptr) {
                    result.event.destroy(result.event.state);
                }
                return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidEvent, 0});
            }
            if (result.event.state == nullptr) {
                if (result.error.code == SubmissionErrorCode::None) {
                    result.error.code = SubmissionErrorCode::SubmissionFailed;
                }
                return std::unexpected(result.error);
            }
            if (result.error.code != SubmissionErrorCode::None) {
                if (result.event.destroy != nullptr) {
                    result.event.destroy(result.event.state);
                }
                return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidEvent, result.error.detail});
            }
            auto event =
                Event::Adopt(control_->service, result.event, control_->payload.backend, control_->payload.device);
            if (!event) {
                const SubmissionErrorCode code = event.error() == EventError::OutOfMemory
                                                     ? SubmissionErrorCode::OutOfMemory
                                                     : SubmissionErrorCode::InvalidEvent;
                return std::unexpected(SubmissionError{code, static_cast<uint64_t>(event.error())});
            }
            return std::move(*event);
        }

        /** @brief Return the number of shared owners, primarily for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return control_.use_count(); }

    private:
        explicit Queue(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_{};
    };

    static_assert(std::is_trivially_copyable_v<QueueId>);
    static_assert(std::is_trivially_copyable_v<QueueCapabilityMask>);
    static_assert(std::is_trivially_copyable_v<QueueView>);
    static_assert(std::is_standard_layout_v<QueueView>);
    static_assert(std::is_trivially_copyable_v<BackendBinding>);
    static_assert(std::is_trivially_copyable_v<BackendScalarBinding>);
    static_assert(std::is_trivially_copyable_v<BackendCommand>);
    static_assert(std::is_standard_layout_v<BackendCommand>);
    static_assert(std::is_trivially_copyable_v<BackendSubmitRequest>);
    static_assert(std::is_trivially_copyable_v<BackendSubmitResult>);
    static_assert(std::is_trivially_copyable_v<BackendQueuePayload>);
    static_assert(std::is_standard_layout_v<BackendSubmitRequest>);
    static_assert(std::is_standard_layout_v<BackendQueuePayload>);

} // namespace Sora::Compute
