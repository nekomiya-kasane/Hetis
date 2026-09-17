/**
 * @file AllocatorTraits.h
 * @brief Standard allocator concepts and rebound allocator projections.
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <memory>
#include <type_traits>

namespace Sora {

    namespace Traits {

        /**
         * @brief Allocator projections named without the redundant @c Allocator word.
         * @details The enclosing @c Traits namespace re-exposes every alias below under its long-form name. The
         * re-exposure qualifies through @c Traits because a template parameter named @c Allocator hides the nested
         * namespace.
         */
        namespace Allocator {

            /** @brief Allocator type rebound to allocate objects of type @p T. */
            template<typename Allocator, typename T>
            using Rebound = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

        } // namespace Allocator

        /** @brief Alias for @ref Sora::Traits::Allocator::Rebound. */
        template<typename Allocator, typename T>
        using ReboundAllocator = Traits::Allocator::Rebound<Allocator, T>;

    } // namespace Traits

    namespace Concept {

        /** @brief Complete cv-unqualified non-array object type. */
        template<typename T>
        concept CompleteObject
            = std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> && requires {
                  sizeof(T);
              };

        /**
         * @brief Allocator concepts named without the redundant @c Allocator word.
         * @details Definitions in this namespace reference each other through unqualified names, because a template
         * parameter named @c Allocator hides the namespace itself in qualified form. The enclosing @c Concept
         * namespace re-exposes every concept below under its long-form name.
         */
        namespace Allocator {

            /** @brief Allocator that can be rebound to a complete object type. */
            template<typename Allocator, typename T>
            concept Rebindable = CompleteObject<T> && requires {
                typename Traits::Allocator::Rebound<Allocator, T>;
                requires std::copy_constructible<Traits::Allocator::Rebound<Allocator, T>>;
                requires std::constructible_from<Traits::Allocator::Rebound<Allocator, T>, const Allocator&>;
            };

            /** @brief Rebound allocator construction that cannot throw. */
            template<typename Allocator, typename T>
            concept NothrowRebindable
                = Rebindable<Allocator, T>
                  && std::is_nothrow_constructible_v<Traits::Allocator::Rebound<Allocator, T>, const Allocator&>;

            /** @brief Allocator that provides allocation and non-throwing deallocation for exactly @p T. */
            template<typename Allocator, typename T>
            concept For = CompleteObject<T> && requires {
                typename std::allocator_traits<Allocator>::value_type;
                typename std::allocator_traits<Allocator>::pointer;
            } && std::same_as<typename std::allocator_traits<Allocator>::value_type, T> && requires(Allocator& allocator, typename std::allocator_traits<Allocator>::pointer pointer, std::size_t count) {
                {
                    std::allocator_traits<Allocator>::allocate(allocator, count)
                } -> std::same_as<typename std::allocator_traits<Allocator>::pointer>;
                { std::allocator_traits<Allocator>::deallocate(allocator, pointer, count) } noexcept;
            };

            /** @brief Allocator construction of @p T from @p Args. */
            template<typename Allocator, typename T, typename... Args>
            concept Constructible = For<Allocator, T> && requires(Allocator& allocator, T* address, Args&&... args) {
                std::allocator_traits<Allocator>::construct(allocator, address, static_cast<Args &&>(args)...);
            };

            /** @brief Allocator destruction of @p T. */
            template<typename Allocator, typename T>
            concept Destroyable = For<Allocator, T> && requires(Allocator& allocator, T* address) {
                std::allocator_traits<Allocator>::destroy(allocator, address);
            };

            /** @brief Allocator destruction of @p T that cannot throw. */
            template<typename Allocator, typename T>
            concept NothrowDestroyable =
                Destroyable<Allocator, T> && std::is_nothrow_destructible_v<T> &&
                (!requires(Allocator& allocator, T* address) { allocator.destroy(address); } ||
                 requires(Allocator& allocator, T* address) { { allocator.destroy(address) } noexcept; });

            /** @brief Allocator whose pointer can be converted to the address of its allocated object. */
            template<typename Allocator, typename T>
            concept AddressableFor
                = For<Allocator, T> && requires(typename std::allocator_traits<Allocator>::pointer pointer) {
                      { std::to_address(pointer) } -> std::same_as<T*>;
                  };

            /** @brief Allocator whose pointer representation is the raw pointer required by an ABI boundary. */
            template<typename Allocator, typename T>
            concept RawPointerFor
                = For<Allocator, T> && std::same_as<typename std::allocator_traits<Allocator>::pointer, T*>;

            /** @brief Allocator rebound to @p T with a raw pointer representation. */
            template<typename Allocator, typename T>
            concept RawPointerRebindable
                = Rebindable<Allocator, T> && RawPointerFor<Traits::Allocator::Rebound<Allocator, T>, T>;

        } // namespace Allocator

        /** @brief Concept alias for @ref Sora::Concept::Allocator::Rebindable. */
        template<typename Allocator, typename T>
        concept RebindableAllocator = Concept::Allocator::Rebindable<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::NothrowRebindable. */
        template<typename Allocator, typename T>
        concept NothrowRebindableAllocator = Concept::Allocator::NothrowRebindable<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::For. */
        template<typename Allocator, typename T>
        concept AllocatorFor = Concept::Allocator::For<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::Constructible. */
        template<typename Allocator, typename T, typename... Args>
        concept AllocatorConstructible = Concept::Allocator::Constructible<Allocator, T, Args...>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::Destroyable. */
        template<typename Allocator, typename T>
        concept AllocatorDestroyable = Concept::Allocator::Destroyable<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::NothrowDestroyable. */
        template<typename Allocator, typename T>
        concept NothrowAllocatorDestroyable = Concept::Allocator::NothrowDestroyable<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::AddressableFor. */
        template<typename Allocator, typename T>
        concept AddressableAllocatorFor = Concept::Allocator::AddressableFor<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::RawPointerFor. */
        template<typename Allocator, typename T>
        concept RawPointerAllocatorFor = Concept::Allocator::RawPointerFor<Allocator, T>;

        /** @brief Concept alias for @ref Sora::Concept::Allocator::RawPointerRebindable. */
        template<typename Allocator, typename T>
        concept RawPointerRebindableAllocator = Concept::Allocator::RawPointerRebindable<Allocator, T>;

    } // namespace Concept

} // namespace Sora
