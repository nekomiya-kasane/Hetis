/**
 * @file Allocation.h
 * @brief Shared ownership for one backend physical-memory allocation.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Memory/Memory.h>

#include <bit>
#include <cassert>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    using AllocationDestroy = void (*)(void* state) noexcept;

    /** @brief ABI payload transferring ownership of one backend allocation to the runtime. */
    struct AllocationPayload {
        MemoryLocation location{};
        PhysicalMemoryId memory{};
        MemoryPropertyMask properties{};
        uint64_t memoryOffset = 0;
        uint64_t sizeBytes = 0;
        uint64_t alignment = 0;
        void* state = nullptr;
        AllocationDestroy destroy = nullptr;
    };

    /** @brief Non-owning physical-allocation view passed to a backend queue. */
    struct AllocationView {
        MemoryLocation location{};
        PhysicalMemoryId memory{};
        MemoryPropertyMask properties{};
        uint64_t memoryOffset = 0;
        uint64_t sizeBytes = 0;
        uint64_t alignment = 0;
        void* state = nullptr;
        BackendServiceId service{};

        /** @brief Return whether every identity and physical range field is internally consistent. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return location.Valid() && memory.Valid() && properties.Valid() && sizeBytes != 0 &&
                   std::has_single_bit(alignment) && memoryOffset <= UINT64_MAX - sizeBytes && state != nullptr &&
                   service.Valid();
        }

        /** @brief Return whether this view denotes a live, internally consistent allocation. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Valid(); }

        /** @brief Return whether [@p offset, @p offset + @p size) lies in this allocation. */
        [[nodiscard]] constexpr bool Covers(uint64_t offset, uint64_t size) const noexcept {
            return offset <= sizeBytes && size <= sizeBytes - offset;
        }

        /** @brief Resolve one allocation-relative interval into the canonical physical storage range. */
        [[nodiscard]] constexpr std::optional<PhysicalMemoryRange> PhysicalRange(uint64_t offset,
                                                                                 uint64_t size) const noexcept {
            if (!Valid() || !Covers(offset, size) || size == 0 || offset > UINT64_MAX - memoryOffset) {
                return std::nullopt;
            }
            const auto bytes = ByteRegion::Make(memoryOffset + offset, size);
            return bytes ? std::optional<PhysicalMemoryRange>{{memory, *bytes}} : std::nullopt;
        }

        friend constexpr bool operator==(AllocationView, AllocationView) noexcept = default;
    };

    /** @brief Failure while adopting an allocation payload. */
    enum class AllocationError : uint8_t {
        InvalidPayload,
        OutOfMemory,
    };

    namespace Detail {

        inline void ReleaseAllocationPayload(AllocationPayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

    } // namespace Detail

    /** @brief Shared RAII ownership for one backend physical-memory allocation. */
    class Allocation {
        struct Control {
            Control(AllocationPayload value, BackendService serviceOwner) noexcept
                : payload(value), service(std::move(serviceOwner)) {}
            ~Control() noexcept { payload.destroy(payload.state); }

            AllocationPayload payload;
            BackendService service;
        };

    public:
        Allocation() noexcept = default;

        /** @brief Adopt @p payload and arrange exactly-once destruction. */
        [[nodiscard]] static std::expected<Allocation, AllocationError> Adopt(BackendService service,
                                                                              AllocationPayload payload) noexcept {
            const bool valid = payload.location.Valid() && payload.memory.Valid() && payload.state != nullptr &&
                               payload.properties.Valid() && payload.destroy != nullptr && payload.sizeBytes != 0 &&
                               payload.memoryOffset <= UINT64_MAX - payload.sizeBytes &&
                               std::has_single_bit(payload.alignment) && service;
            if (!valid) {
                Detail::ReleaseAllocationPayload(payload);
                return std::unexpected(AllocationError::InvalidPayload);
            }
            try {
                return Allocation{std::make_shared<Control>(payload, std::move(service))};
            } catch (const std::bad_alloc&) {
                Detail::ReleaseAllocationPayload(payload);
                return std::unexpected(AllocationError::OutOfMemory);
            }
        }

        /** @brief Return whether this handle owns a live allocation. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return a non-owning backend view. */
        [[nodiscard]] AllocationView View() const noexcept {
            if (control_ == nullptr) {
                return {};
            }
            const AllocationPayload& payload = control_->payload;
            return {.location = payload.location,
                    .memory = payload.memory,
                    .properties = payload.properties,
                    .memoryOffset = payload.memoryOffset,
                    .sizeBytes = payload.sizeBytes,
                    .alignment = payload.alignment,
                    .state = payload.state,
                    .service = control_->service.Id()};
        }

        /** @brief Return the allocation's coherence domain and backend addressability. @pre This handle is non-empty.
         */
        [[nodiscard]] MemoryLocation Location() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.location;
        }

        /** @brief Return the allocation size in bytes. @pre This handle is non-empty. */
        [[nodiscard]] uint64_t SizeBytes() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.sizeBytes;
        }

        /** @brief Return the guaranteed byte alignment. @pre This handle is non-empty. */
        [[nodiscard]] uint64_t Alignment() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.alignment;
        }

        /** @brief Return the number of shared owners, primarily for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return control_.use_count(); }

    private:
        explicit Allocation(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_{};
    };

    static_assert(std::is_trivially_copyable_v<AllocationPayload>);
    static_assert(std::is_standard_layout_v<AllocationPayload>);
    static_assert(std::is_trivially_copyable_v<AllocationView>);

} // namespace Sora::Compute
