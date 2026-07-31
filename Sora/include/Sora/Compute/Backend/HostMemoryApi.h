/**
 * @file HostMemoryApi.h
 * @brief Versioned coherent host-memory mapping ABI and validated runtime facade.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Abi.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Memory/Allocation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Requested host access to one coherent allocation interval. */
    enum class HostMemoryAccess : uint8_t {
        Read,
        Write,
    };

    /** @brief Stable coherent-mapping failure independent of a backend SDK. */
    enum class HostMemoryErrorCode : uint8_t {
        None,
        InvalidApi,
        InvalidRequest,
        ForeignAllocation,
        UnsupportedAccess,
        MappingFailed,
        InvalidResult,
    };

    /** @brief Host mapping failure with an opaque backend diagnostic identity. */
    struct HostMemoryError {
        HostMemoryErrorCode code = HostMemoryErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(HostMemoryError, HostMemoryError) noexcept = default;
    };

    /** @brief Allocation-free request for one exact coherent host-memory interval. */
    struct BackendHostMemoryRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendHostMemoryRequest);
        MemoryLocation location{};
        AllocationView allocation{};
        uint64_t offset = 0;
        uint64_t sizeBytes = 0;
        HostMemoryAccess access = HostMemoryAccess::Read;
    };

    /** @brief Borrowed mapping result whose lifetime remains bounded by the source allocation. */
    struct BackendHostMemoryResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendHostMemoryResult);
        std::byte* address = nullptr;
        uint64_t sizeBytes = 0;
        HostMemoryError error{HostMemoryErrorCode::MappingFailed, 0};
    };

    using BackendMapHostMemory = void (*)(void* state, const BackendHostMemoryRequest* request,
                                          BackendHostMemoryResult* result) noexcept;

    /** @brief Version-1 POD callback table for one host-visible coherent memory location. */
    struct BackendHostMemoryApiV1 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendHostMemoryApiV1);
        MemoryLocation location{};
        void* state = nullptr;
        BackendMapHostMemory map = nullptr;
    };

    namespace Detail {

        inline constexpr MemoryPropertyMask kCoherentHostMemoryProperties =
            MemoryProperty::HostVisible | MemoryProperty::HostCoherent;

        [[nodiscard]] constexpr bool ValidHostMemoryAccess(HostMemoryAccess access) noexcept {
            return access == HostMemoryAccess::Read || access == HostMemoryAccess::Write;
        }

        [[nodiscard]] constexpr bool ValidHostMemoryErrorCode(HostMemoryErrorCode code) noexcept {
            return code == HostMemoryErrorCode::None || code == HostMemoryErrorCode::InvalidApi ||
                   code == HostMemoryErrorCode::InvalidRequest || code == HostMemoryErrorCode::ForeignAllocation ||
                   code == HostMemoryErrorCode::UnsupportedAccess || code == HostMemoryErrorCode::MappingFailed ||
                   code == HostMemoryErrorCode::InvalidResult;
        }

    } // namespace Detail

    /** @brief Validated authority for borrowing coherent host spans from one exact service and location. */
    class HostMemoryAccessor final {
    public:
        /** @brief Validate and retain one immutable coherent-mapping callback table. */
        [[nodiscard]] static std::expected<HostMemoryAccessor, HostMemoryError>
        Create(BackendService service, BackendHostMemoryApiV1 api) noexcept {
            if (!service || !ValidBackendAbi(api.abiVersion, api.structSize, sizeof(BackendHostMemoryApiV1)) ||
                !api.location.Valid() || api.state == nullptr || api.map == nullptr) {
                return std::unexpected(HostMemoryError{HostMemoryErrorCode::InvalidApi, 0});
            }
            return HostMemoryAccessor{std::move(service), api};
        }

        /** @brief Return the service whose allocations this accessor accepts. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return the exact host-visible coherent location accepted by this accessor. */
        [[nodiscard]] constexpr MemoryLocation Location() const noexcept { return api_.location; }

        /** @brief Retain the destination service while composing a cross-service transfer engine. */
        [[nodiscard]] BackendService RetainService() const noexcept { return service_; }

        /** @brief Borrow a read-only span while the allocation owner remains live and externally synchronized. */
        [[nodiscard]] std::expected<std::span<const std::byte>, HostMemoryError>
        Read(const Allocation& allocation, uint64_t offset = 0, uint64_t sizeBytes = UINT64_MAX) const noexcept {
            return Read(allocation.View(), offset, sizeBytes);
        }

        /** @brief Borrow a mutable span while the allocation owner remains live and externally synchronized. */
        [[nodiscard]] std::expected<std::span<std::byte>, HostMemoryError>
        Write(const Allocation& allocation, uint64_t offset = 0, uint64_t sizeBytes = UINT64_MAX) const noexcept {
            return Write(allocation.View(), offset, sizeBytes);
        }

        /** @brief Borrow a read-only span from a view whose owning Allocation is retained by the caller. */
        [[nodiscard]] std::expected<std::span<const std::byte>, HostMemoryError>
        Read(AllocationView allocation, uint64_t offset, uint64_t sizeBytes) const noexcept {
            auto mapped = Map(allocation, offset, sizeBytes, HostMemoryAccess::Read);
            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            return std::span<const std::byte>{mapped->data(), mapped->size()};
        }

        /** @brief Borrow a mutable span from a view whose owning Allocation is retained by the caller. */
        [[nodiscard]] std::expected<std::span<std::byte>, HostMemoryError>
        Write(AllocationView allocation, uint64_t offset, uint64_t sizeBytes) const noexcept {
            return Map(allocation, offset, sizeBytes, HostMemoryAccess::Write);
        }

    private:
        [[nodiscard]] std::expected<std::span<std::byte>, HostMemoryError>
        Map(AllocationView allocation, uint64_t offset, uint64_t sizeBytes, HostMemoryAccess access) const noexcept {
            if (!allocation || allocation.service != service_.Id() || allocation.location != api_.location ||
                !allocation.properties.Contains(Detail::kCoherentHostMemoryProperties) ||
                !Detail::ValidHostMemoryAccess(access)) {
                return std::unexpected(HostMemoryError{HostMemoryErrorCode::ForeignAllocation, 0});
            }
            const uint64_t available = allocation.sizeBytes - std::min(offset, allocation.sizeBytes);
            const uint64_t resolvedSize = sizeBytes == UINT64_MAX ? available : sizeBytes;
            if (resolvedSize == 0 || resolvedSize > std::numeric_limits<size_t>::max() ||
                !allocation.Covers(offset, resolvedSize)) {
                return std::unexpected(HostMemoryError{HostMemoryErrorCode::InvalidRequest, resolvedSize});
            }
            const BackendHostMemoryRequest request{.location = api_.location,
                                                   .allocation = allocation,
                                                   .offset = offset,
                                                   .sizeBytes = resolvedSize,
                                                   .access = access};
            BackendHostMemoryResult result{};
            api_.map(api_.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendHostMemoryResult)) ||
                !Detail::ValidHostMemoryErrorCode(result.error.code)) {
                return std::unexpected(HostMemoryError{HostMemoryErrorCode::InvalidResult, 0});
            }
            if (result.error.code != HostMemoryErrorCode::None) {
                if (result.address != nullptr || result.sizeBytes != 0) {
                    return std::unexpected(HostMemoryError{HostMemoryErrorCode::InvalidResult, result.sizeBytes});
                }
                return std::unexpected(result.error);
            }
            if (result.address == nullptr || result.sizeBytes != resolvedSize) {
                return std::unexpected(HostMemoryError{HostMemoryErrorCode::InvalidResult, result.sizeBytes});
            }
            return std::span<std::byte>{result.address, static_cast<size_t>(result.sizeBytes)};
        }

        HostMemoryAccessor(BackendService service, BackendHostMemoryApiV1 api) noexcept
            : service_(std::move(service)), api_(api) {}

        BackendService service_{};
        BackendHostMemoryApiV1 api_{};
    };

    static_assert(std::is_trivially_copyable_v<BackendHostMemoryRequest>);
    static_assert(std::is_trivially_copyable_v<BackendHostMemoryResult>);
    static_assert(std::is_trivially_copyable_v<BackendHostMemoryApiV1>);
    static_assert(std::is_standard_layout_v<BackendHostMemoryRequest>);
    static_assert(std::is_standard_layout_v<BackendHostMemoryResult>);
    static_assert(std::is_standard_layout_v<BackendHostMemoryApiV1>);

} // namespace Sora::Compute
