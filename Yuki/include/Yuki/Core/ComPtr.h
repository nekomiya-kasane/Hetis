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
 *
 * Acquire/Release are declared in RootObject.h (D12 hooks); T12 will replace the temporary
 * forwarder bodies with full hierarchical D8/D9/D10/D13 semantics.
 */
#include <Yuki/Core/RootObject.h>
#include <utility>

namespace Yuki {

    template<class T>
    class ComPtr {
        T* p_{};

      public:
        /// Default-construct to null (no allocation, no refcount touch).
        constexpr ComPtr() noexcept = default;

        /// Construct from a raw pointer, bumping the refcount by one (defensive copy).
        explicit ComPtr(T* p) noexcept : p_(p) { if (p_) Acquire(p_); }

        /// Adopt a raw pointer that already carries a +1 refcount — does NOT bump.
        /// @note Use only when the source owns the +1 (factory return, Detach, MakeOwned).
        ///       Wrapping a borrowed or observer pointer via Adopt silently under-counts —
        ///       use the explicit ComPtr(T*) ctor for that case.
        static ComPtr Adopt(T* p) noexcept {
            ComPtr c;
            c.p_ = p;
            return c;
        }

        /// Copy: bumps the refcount on the shared object.
        ComPtr(const ComPtr& o) noexcept : p_(o.p_) { if (p_) Acquire(p_); }

        /// Move: steals the pointer; no refcount change.
        ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

        /// Copy-and-swap assignment (parameter BY VALUE is intentional — constructs via
        /// the copy or move ctor, then swaps with the temporary which dies and Releases
        /// the old pointee). Handles self-assignment and move-assignment with no branch.
        /// @note Do NOT "optimise" this to `const ComPtr&` — that loses the move-assignment
        ///       overload and the exception-safety guarantee in one stroke.
        ComPtr& operator=(ComPtr o) noexcept {
            std::swap(p_, o.p_);
            return *this;
        }

        /// Destructor: Release the refcount; delete only if 0-transition.
        ~ComPtr() noexcept { if (p_ && Release(p_)) delete p_; }

        T*  Get()    const noexcept { return p_; }                         ///< Raw pointer accessor.
        T*  Detach()       noexcept { return std::exchange(p_, nullptr); } ///< Surrender ownership.
        T&  operator*()  const noexcept { return *p_; }
        T*  operator->() const noexcept { return p_; }
        explicit operator bool() const noexcept { return p_ != nullptr; }
    };

} // namespace Yuki
