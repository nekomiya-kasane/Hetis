#pragma once
/**
 * @file ComPtr.h
 * @brief D12 strict-ownership intrusive smart pointer over RootObject-derived T.
 *
 * Design notes:
 * - One-word storage (a T*); the intrusive refcount lives in T's TaggedPayload metaword.
 * - Two construction modes — the asymmetry is the most error-prone part of intrusive
 *   smart pointers, so pick deliberately:
 *     - explicit ComPtr(T*)  — bumps the refcount. Use when wrapping a borrowed/observer
 *                               pointer whose +1 you do NOT own (e.g., aliasing an existing
 *                               handle, defensive copy from a caller-owned reference).
 *     - static Adopt(T*)     — consumes an existing +1 without bumping. Use ONLY when the
 *                               source already holds a +1 on your behalf (factory return,
 *                               Detach, MakeOwned). MakeOwned (T11) is the canonical caller;
 *                               using Adopt on a borrowed pointer silently under-counts.
 * - The copy-and-swap operator=(ComPtr o) is exception-safe without a manual swap helper.
 * - The destructor calls Release(p_) and only delete's if that returns true (0-transition).
 * - Closure cross-cast conversions (ComPtr<From> -> ComPtr<T>): a QueryInterface-shaped
 *   navigation between facets of one object closure. The resolution is driven entirely by the
 *   MetaClass — meta-identity (@c MetaCore::iid) for a concrete-type target, the dispatch
 *   tables (@ref QueryDynamicRaw) for an interface target — so NO RTTI / @c dynamic_cast is
 *   used. The conversion succeeds iff T is reachable as a facet of the SAME nucleus as the
 *   source and yields null otherwise — never a throw. See the converting ctors below.
 *
 * Acquire/Release are declared in RootObject.h (D12 hooks); T12 will replace the temporary
 * forwarder bodies with full hierarchical D8/D9/D10/D13 semantics.
 */
#include <Yuki/Core/RootObject.h>

#include <type_traits>
#include <utility>

namespace Yuki {

    namespace Detail {
        /// @brief Type-erased closure cross-cast kernel (defined in Query.cpp).
        ///
        /// Resolves the facet identified by @p target within the closure of @p from, driven
        /// purely by the MetaClass — meta-identity (@c MetaCore::iid) for a concrete-class
        /// target, the merged dispatch tables for an interface target — so it never consults
        /// RTTI. Roots the search at @c Nucleus(from) so any facet of the shared closure (the
        /// nucleus impl, its materialized facades, its extensions) is reachable.
        ///
        /// @return A @c RootObject* carrying a fresh @b +1 the caller owns (Adopt-shaped), or
        ///         @c nullptr when no such facet exists in the closure. Null-tolerant on input.
        [[nodiscard]] RootObject* CrossCastOwned(RootObject* from, Iid target) noexcept;
    } // namespace Detail

    template<class T>
    class ComPtr {
        T* p_{};

        // Cross-template access to p_ so the closure cross-cast ctors can read/empty a
        // ComPtr<From> whose element type differs from T.
        template<class U>
        friend class ComPtr;

    public:
        /// Default-construct to null (no allocation, no refcount touch).
        constexpr ComPtr() noexcept = default;

        /// Construct from a raw pointer, bumping the refcount by one (defensive copy).
        explicit ComPtr(T* p) noexcept : p_(p) {
            if (p_) {
                Acquire(p_);
            }
        }

        /// Adopt a raw pointer that already carries a +1 refcount — does NOT bump.
        /// @note Use only when the source owns the +1 (factory return, Detach, MakeOwned).
        ///       Wrapping a borrowed or observer pointer via Adopt silently under-counts —
        ///       use the explicit ComPtr(T*) ctor for that case.
        [[nodiscard]] static ComPtr Adopt(T* p) noexcept {
            ComPtr c;
            c.p_ = p;
            return c;
        }

        /// Copy: bumps the refcount on the shared object.
        ComPtr(const ComPtr& o) noexcept : p_(o.p_) {
            if (p_) {
                Acquire(p_);
            }
        }

        /// Move: steals the pointer; no refcount change.
        ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

        // -----------------------------------------------------------------------------------------
        // Closure cross-cast conversions (ComPtr<From> -> ComPtr<T>).
        //
        // Semantics: succeed iff T is reachable as a facet of the SAME closure (same nucleus) as the
        // source's pointee — the source node itself (up/down cast), or a sibling facade / extension
        // of the shared nucleus. On a miss the result is null (QueryInterface shape), never a throw.
        // Resolution is MetaClass-driven (meta-identity + dispatch tables) — never @c dynamic_cast.
        // Converting assignment falls out for free through the by-value copy-and-swap operator= once
        // these (implicit) ctors exist.
        //
        // Refcount discipline:
        //   - Upcast (T is a base of From): the facet IS the same RootObject; static_cast adjusts the
        //     pointer with zero runtime cost. Copy bumps once; move steals the source's +1 outright.
        //   - Down / cross / sibling cast: routed through @ref Detail::CrossCastOwned, which returns a
        //     fresh +1 the new ComPtr owns. Copy keeps the source intact. Move additionally drops the
        //     source's +1 (deleting the source pointee on a 0-transition). A failed move leaves the
        //     source intact, so an implicit conversion can never silently destroy a live object.
        // -----------------------------------------------------------------------------------------
        template<class From>
            requires(!std::is_same_v<From, T> && std::is_base_of_v<RootObject, From> &&
                     std::is_base_of_v<RootObject, T>)
        ComPtr(const ComPtr<From>& o) noexcept {
            if constexpr (std::is_convertible_v<From*, T*>) {
                p_ = static_cast<T*>(o.p_);  // base subobject — same RootObject, no closure walk.
                if (p_) {
                    Acquire(p_);
                }
            } else {
                // CrossCastOwned hands back a fresh +1; take it as-is (no extra Acquire).
                p_ = static_cast<T*>(Detail::CrossCastOwned(static_cast<RootObject*>(o.p_), IidOf<T>()));
            }
        }

        template<class From>
            requires(!std::is_same_v<From, T> && std::is_base_of_v<RootObject, From> &&
                     std::is_base_of_v<RootObject, T>)
        ComPtr(ComPtr<From>&& o) noexcept {
            if constexpr (std::is_convertible_v<From*, T*>) {
                // Base subobject — same RootObject; steal the source's existing +1.
                p_ = static_cast<T*>(std::exchange(o.p_, nullptr));
            } else {
                p_ = static_cast<T*>(Detail::CrossCastOwned(static_cast<RootObject*>(o.p_), IidOf<T>()));
                if (!p_) {
                    return;  // miss: leave the source intact rather than consuming it.
                }
                // Hit: we now own a fresh +1 on the facet — consume the source's reference. For a
                // same-object cast this is a balanced +1/-1; the object survives because the
                // CrossCastOwned acquire happened before this release.
                From* src = std::exchange(o.p_, nullptr);
                if (Release(src)) {
                    delete src;
                }
            }
        }

        /// Copy-and-swap assignment (parameter BY VALUE is intentional — constructs via
        /// the copy or move ctor, then swaps with the temporary which dies and Releases
        /// the old pointee). Handles self-assignment and move-assignment with no branch.
        /// Also absorbs cross-type assignment: a ComPtr<From> argument materialises the
        /// by-value ComPtr<T> through the closure cross-cast ctor above.
        /// @note Do NOT "optimise" this to `const ComPtr&` — that loses the move-assignment
        ///       overload and the exception-safety guarantee in one stroke.
        ComPtr& operator=(ComPtr o) noexcept {
            std::swap(p_, o.p_);
            return *this;
        }

        /// Destructor: Release the refcount; delete only if 0-transition.
        ~ComPtr() noexcept {
            if (p_ && Release(p_)) {
                delete p_;
            }
        }

        T* Get() const noexcept { return p_; }                                    ///< Raw pointer accessor.
        [[nodiscard]] T* Detach() noexcept { return std::exchange(p_, nullptr); } ///< Surrender ownership.
        T& operator*() const noexcept { return *p_; }
        T* operator->() const noexcept { return p_; }
        explicit operator bool() const noexcept { return p_ != nullptr; }
    };

} // namespace Yuki
