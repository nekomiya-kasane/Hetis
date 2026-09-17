/**
 * @file AllocateUnique.h
 * @brief Allocate and uniquely own one object through a standard allocator.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Guard.h>
#include <Sora/Core/Memory/AllocatorTraits.h>
#include <Sora/Platform.h>

#include <memory>
#include <utility>

namespace Sora {

    namespace Detail {

        /** @brief Deleter retaining allocator provenance for one allocated object. */
        template<typename Allocator>
            requires Concept::AddressableAllocatorFor<Allocator,
                                                      typename std::allocator_traits<Allocator>::value_type> &&
                     Concept::NothrowAllocatorDestroyable<Allocator,
                                                          typename std::allocator_traits<Allocator>::value_type>
        struct AllocatorDeleter {
            using ValueType = typename std::allocator_traits<Allocator>::value_type;
            using pointer = typename std::allocator_traits<Allocator>::pointer;

            constexpr void operator()(pointer allocation) noexcept {
                auto* address = std::to_address(allocation);
                std::allocator_traits<Allocator>::destroy(allocator_, address);
                std::allocator_traits<Allocator>::deallocate(allocator_, allocation, 1);
            }

            SORA_NO_UNIQUE_ADDRESS Allocator allocator_;
        };

    } // namespace Detail

    /**
     * @brief Allocate and construct one object while retaining allocator provenance for destruction.
     * @tparam T Complete object type to construct.
     * @tparam Allocator Source allocator type, rebound to @p T before allocation.
     * @tparam Args Constructor argument types.
     * @param[in] allocator Source allocator whose allocation domain owns the result.
     * @param[in] args Arguments forwarded to the constructor of @p T.
     * @return A unique owner whose deleter destroys and deallocates through the rebound allocator.
     */
    template<typename T, typename Allocator, typename... Args>
        requires Concept::RebindableAllocator<Allocator, T> &&
                 Concept::AddressableAllocatorFor<Traits::ReboundAllocator<Allocator, T>, T> &&
                 Concept::AllocatorConstructible<Traits::ReboundAllocator<Allocator, T>, T, Args...> &&
                 Concept::NothrowAllocatorDestroyable<Traits::ReboundAllocator<Allocator, T>, T> &&
                 std::is_nothrow_move_constructible_v<Traits::ReboundAllocator<Allocator, T>>
    [[nodiscard]] constexpr auto AllocateUnique(const Allocator& allocator, Args&&... args)
        -> std::unique_ptr<T, Detail::AllocatorDeleter<Traits::ReboundAllocator<Allocator, T>>> {
        using ReboundAllocator = Traits::ReboundAllocator<Allocator, T>;
        using TraitsType = std::allocator_traits<ReboundAllocator>;
        using Pointer = typename TraitsType::pointer;
        using Deleter = Detail::AllocatorDeleter<ReboundAllocator>;

        ReboundAllocator rebound{allocator};
        Pointer pointer = TraitsType::allocate(rebound, 1);
        ScopeFail rollback{[&]() noexcept { TraitsType::deallocate(rebound, pointer, 1); }};
        TraitsType::construct(rebound, std::to_address(pointer), static_cast<Args&&>(args)...);
        Deleter deleter{std::move(rebound)};
        rollback.Release();
        return {pointer, std::move(deleter)};
    }

} // namespace Sora
