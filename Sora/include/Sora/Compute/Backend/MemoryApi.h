/**
 * @file MemoryApi.h
 * @brief Versioned backend allocation ABI and validated runtime facade.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Abi.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Memory/Allocation.h>

#include <bit>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Stable allocation failure independent of a concrete driver SDK. */
    enum class MemoryAllocationErrorCode : uint8_t {
        None,
        InvalidApi,
        InvalidRequest,
        LocationUnavailable,
        UnsupportedProperties,
        AllocationFailed,
        InvalidAllocation,
        OutOfMemory,
        DeviceLost,
    };

    /** @brief Allocation failure with an opaque backend diagnostic identity. */
    struct MemoryAllocationError {
        MemoryAllocationErrorCode code = MemoryAllocationErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(MemoryAllocationError, MemoryAllocationError) noexcept = default;
    };

    /** @brief Allocation-free request for physical storage at one exact memory location. */
    struct BackendAllocationRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendAllocationRequest);
        MemoryLocation location{};
        MemoryPropertyMask requiredProperties{};
        MemoryPropertyMask preferredProperties{};
        uint64_t sizeBytes = 0;
        uint64_t alignment = 1;
    };

    /** @brief Runtime-allocated callback result transferring one physical allocation on success. */
    struct BackendAllocationResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendAllocationResult);
        AllocationPayload allocation{};
        MemoryAllocationError error{MemoryAllocationErrorCode::AllocationFailed, 0};
    };

    using BackendAllocate = void (*)(void* state, const BackendAllocationRequest* request,
                                     BackendAllocationResult* result) noexcept;

    /** @brief Version-1 POD callback table implemented by one backend memory location. */
    struct BackendMemoryApiV1 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendMemoryApiV1);
        MemoryLocation location{};
        MemoryPropertyMask supportedProperties{};
        void* state = nullptr;
        BackendAllocate allocate = nullptr;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidMemoryAllocationErrorCode(MemoryAllocationErrorCode code) noexcept {
            return code == MemoryAllocationErrorCode::None || code == MemoryAllocationErrorCode::InvalidApi ||
                   code == MemoryAllocationErrorCode::InvalidRequest ||
                   code == MemoryAllocationErrorCode::LocationUnavailable ||
                   code == MemoryAllocationErrorCode::UnsupportedProperties ||
                   code == MemoryAllocationErrorCode::AllocationFailed ||
                   code == MemoryAllocationErrorCode::InvalidAllocation ||
                   code == MemoryAllocationErrorCode::OutOfMemory || code == MemoryAllocationErrorCode::DeviceLost;
        }

        [[nodiscard]] constexpr bool ValidAllocationRequest(const BackendAllocationRequest& request,
                                                            const BackendMemoryApiV1& api) noexcept {
            return ValidBackendAbi(request.abiVersion, request.structSize, sizeof(BackendAllocationRequest)) &&
                   request.location == api.location && request.sizeBytes != 0 &&
                   std::has_single_bit(request.alignment) && request.requiredProperties.Valid() &&
                   request.preferredProperties.Valid() && api.supportedProperties.Contains(request.requiredProperties);
        }

        [[nodiscard]] constexpr bool AllocationSatisfies(const AllocationPayload& payload,
                                                         const BackendAllocationRequest& request) noexcept {
            return payload.location == request.location && payload.memory.Valid() && payload.properties.Valid() &&
                   payload.properties.Contains(request.requiredProperties) && payload.sizeBytes >= request.sizeBytes &&
                   payload.alignment >= request.alignment && std::has_single_bit(payload.alignment) &&
                   payload.memoryOffset <= UINT64_MAX - payload.sizeBytes && payload.state != nullptr &&
                   payload.destroy != nullptr;
        }

    } // namespace Detail

    /** @brief Validated allocation facade over one versioned backend memory callback table. */
    class MemoryAllocator final {
    public:
        /** @brief Validate and retain a copy of @p api's immutable callback table. */
        [[nodiscard]] static std::expected<MemoryAllocator, MemoryAllocationError>
        Create(BackendService service, BackendMemoryApiV1 api) noexcept {
            if (!ValidBackendAbi(api.abiVersion, api.structSize, sizeof(BackendMemoryApiV1)) || !api.location.Valid() ||
                !api.supportedProperties.Valid() || api.allocate == nullptr || !service) {
                return std::unexpected(MemoryAllocationError{MemoryAllocationErrorCode::InvalidApi, 0});
            }
            return MemoryAllocator{std::move(service), api};
        }

        /** @brief Return the exact addressability location served by this allocator. */
        [[nodiscard]] constexpr MemoryLocation Location() const noexcept { return api_.location; }

        /** @brief Return the backend service owning allocations produced by this facade. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return the complete property set supported by this allocation domain. */
        [[nodiscard]] constexpr MemoryPropertyMask SupportedProperties() const noexcept {
            return api_.supportedProperties;
        }

        /** @brief Allocate and transactionally adopt storage satisfying every hard request constraint. */
        [[nodiscard]] std::expected<Allocation, MemoryAllocationError>
        Allocate(const BackendAllocationRequest& request) const noexcept {
            if (!Detail::ValidAllocationRequest(request, api_)) {
                return std::unexpected(MemoryAllocationError{MemoryAllocationErrorCode::InvalidRequest, 0});
            }

            BackendAllocationResult result{};
            api_.allocate(api_.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendAllocationResult))) {
                Detail::ReleaseAllocationPayload(result.allocation);
                return std::unexpected(MemoryAllocationError{MemoryAllocationErrorCode::InvalidAllocation, 0});
            }
            if (!Detail::ValidMemoryAllocationErrorCode(result.error.code)) {
                Detail::ReleaseAllocationPayload(result.allocation);
                return std::unexpected(MemoryAllocationError{MemoryAllocationErrorCode::InvalidAllocation, 0});
            }
            if (result.allocation.state == nullptr) {
                if (result.error.code == MemoryAllocationErrorCode::None) {
                    result.error.code = MemoryAllocationErrorCode::AllocationFailed;
                }
                return std::unexpected(result.error);
            }
            if (result.error.code != MemoryAllocationErrorCode::None ||
                !Detail::AllocationSatisfies(result.allocation, request)) {
                Detail::ReleaseAllocationPayload(result.allocation);
                return std::unexpected(
                    MemoryAllocationError{MemoryAllocationErrorCode::InvalidAllocation, result.error.detail});
            }

            auto allocation = Allocation::Adopt(service_, result.allocation);
            if (!allocation) {
                const MemoryAllocationErrorCode code = allocation.error() == AllocationError::OutOfMemory
                                                           ? MemoryAllocationErrorCode::OutOfMemory
                                                           : MemoryAllocationErrorCode::InvalidAllocation;
                return std::unexpected(MemoryAllocationError{code, static_cast<uint64_t>(allocation.error())});
            }
            return std::move(*allocation);
        }

    private:
        MemoryAllocator(BackendService service, BackendMemoryApiV1 api) noexcept
            : service_(std::move(service)), api_(api) {}

        BackendService service_{};
        BackendMemoryApiV1 api_{};
    };

    static_assert(std::is_trivially_copyable_v<BackendAllocationRequest>);
    static_assert(std::is_trivially_copyable_v<BackendAllocationResult>);
    static_assert(std::is_trivially_copyable_v<BackendMemoryApiV1>);
    static_assert(std::is_standard_layout_v<BackendAllocationRequest>);
    static_assert(std::is_standard_layout_v<BackendAllocationResult>);
    static_assert(std::is_standard_layout_v<BackendMemoryApiV1>);

} // namespace Sora::Compute
