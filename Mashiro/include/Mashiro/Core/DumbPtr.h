/**
 * @file DumbPtr.h
 * @brief Non-owning observer pointer — a documented, value-semantic `W*`.
 *
 * @ref Mashiro::DumbPtr is the project's equivalent of the proposed
 * `std::observer_ptr<W>`: a thin wrapper over a raw `W*` that **states intent**
 * — "I observe this object, I do not own it." It never allocates, never frees,
 * has no destructor side effect, and is trivially copyable, so it carries
 * **zero** overhead over a bare pointer. Reach for it wherever a back-reference,
 * a cached non-owning handle, or a parent link would otherwise be spelled with a
 * naked pointer whose ownership story is unclear.
 *
 * @par Semantics (deliberate design choices)
 * - **Non-owning.** No `Release()` / ownership transfer, no destructor. Use
 *   `std::unique_ptr` / `std::shared_ptr` when ownership is the point.
 * - **Identity, not value.** Comparison, ordering, and hashing operate on the
 *   *address*, never by dereferencing the pointee. An observer is identified by
 *   *what it points at*, so these operations never touch a possibly-dangling
 *   object and never recurse on cyclic graphs.
 * - **Pointer-like const (no propagation).** A `const DumbPtr<W>` still yields
 *   mutable `W&` / `W*` — exactly like `W* const`. To observe-as-const, spell
 *   `DumbPtr<const W>`.
 * - **`DumbPtr<void>`** is a well-formed opaque handle (no `operator*` /
 *   `operator->`), mirroring `void*`.
 *
 * @par Framework integration
 * `DumbPtr` is a first-class citizen of the reflection frameworks via identity
 * semantics, with no coupling back into them:
 * - **Hash** — a free @ref Mashiro::HashValue hook (found by ADL) hashes the
 *   address bits, so `Hashing::Hash(p)` and the auto-injected `std::hash` work.
 * - **ToString** — a member @ref DumbPtr::ToString renders `"null"` or
 *   `"DumbPtr<W>(0xADDR)"` (member form avoids a name clash with the
 *   `Mashiro::ToString` CPO, matching the `Hashing::Uuid` convention).
 * - **ToJson** — opt-in, one-way, via the bridge header `DumbPtrJson.h`
 *   (keeps the nlohmann dependency out of this header).
 *
 * This header includes only `<type_traits>`-tier standard headers plus
 * `TypeTraits.h`; it never includes `Hash.h`, `ToString.h`, or `ToJson.h`.
 *
 * @ingroup Core
 */
#pragma once

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <type_traits>

#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

    /**
     * @brief A non-owning observer over a raw `W*`.
     *
     * Wraps a single `W*` with value semantics. Copyable, trivially so; the copy observes the same object. The 
     * handle is the only state — there is no pointee lifetime management.
     *
     * @tparam W The observed (pointed-to) type. May be `void` for an opaque handle, or cv-qualified (e.g. 
     *           `const Foo`) to observe-as-const.
     */
    template<class W>
    class DumbPtr {
        W* ptr_ = nullptr;

    public:
        /// @brief The observed type (the template parameter).
        using ElementType = W;
        /// @brief The raw pointer type, `W*`.
        using Pointer = std::add_pointer_t<W>;

        // ---------------------------------------------------------------------
        // Construction
        // ---------------------------------------------------------------------

        /// @brief Construct an empty (null) observer.
        constexpr DumbPtr() noexcept = default;

        /// @brief Construct an empty (null) observer from `nullptr`.
        constexpr DumbPtr(std::nullptr_t) noexcept {}

        /// @brief Observe the object at @p p. Explicit, mirroring raw-pointer intent.
        constexpr explicit DumbPtr(Pointer p) noexcept : ptr_(p) {}

        /**
         * @brief Converting constructor for pointer-compatible element types.
         *
         * Enabled exactly when `W2*` implicitly converts to `W*`, so it admits derived → base upcasts and 
         * `T*` → `const T*`, and (like `void*`) accepts any object pointer when `W` is `void`. Unrelated 
         * or narrowing conversions are rejected — matching raw-pointer rules. When `W2 == W` the implicitly
         * declared copy constructor is preferred.
         *
         * @tparam W2 Source element type with `W2*` convertible to `W*`.
         */
        template<class W2>
            requires std::convertible_to<W2*, W*>
        constexpr DumbPtr(DumbPtr<W2> other) noexcept : ptr_(other.Get()) {}

        // ---------------------------------------------------------------------
        // Observers (const-qualified, mutable pointee access — no const propagation)
        // ---------------------------------------------------------------------

        /// @brief The observed raw pointer (may be null).
        [[nodiscard]] constexpr Pointer Get() const noexcept { return ptr_; }

        /// @brief Member access on the observed object. Precondition: non-null.
        [[nodiscard]] constexpr Pointer operator->() const noexcept
            requires (!std::is_void_v<W>) { return ptr_; }

        /// @brief Reference to the observed object. Precondition: non-null.
        [[nodiscard]] constexpr std::add_lvalue_reference_t<W> operator*() const noexcept
            requires (!std::is_void_v<W>) { return *ptr_; }

        /// @brief `true` iff this observer is non-null.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return ptr_ != nullptr;
        }

        /// @brief Explicit conversion to the raw pointer.
        [[nodiscard]] constexpr explicit operator Pointer() const noexcept { return ptr_; }

        // ---------------------------------------------------------------------
        // Modifiers (act on the handle, never the pointee)
        // ---------------------------------------------------------------------

        /// @brief Re-point at @p p (default: become null). Does **not** free anything.
        constexpr void Reset(Pointer p = nullptr) noexcept { ptr_ = p; }

        /// @brief Exchange observed pointers with @p other.
        constexpr void Swap(DumbPtr& other) noexcept {
            Pointer t = ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = t;
        }

        // ---------------------------------------------------------------------
        // ToString integration (member form — avoids clashing with the 
        // Mashiro::ToString CPO; picked up by its `.ToString()` branch).
        // ---------------------------------------------------------------------

        /// @brief Identity rendering: `"null"`, or `"DumbPtr<W>(0xADDR)"`.
        ///        Never dereferences the pointee.
        [[nodiscard]] std::string ToString() const {
            if (ptr_ == nullptr) {
                return "null";
            }
            return std::format("DumbPtr<{}>({})", Traits::TypeName<W>, static_cast<const void*>(ptr_));
        }

        // ---------------------------------------------------------------------
        // Comparisons — identity (address) based, total order. Hidden friends so
        // they are found only by ADL and never pollute the namespace.
        // ---------------------------------------------------------------------

        /// @brief Equal iff they observe the same object (or both null).
        [[nodiscard]] friend constexpr bool operator==(DumbPtr a, DumbPtr b) noexcept {
            return a.ptr_ == b.ptr_;
        }

        /// @brief Total order over the observed addresses (`std::strong_ordering`).
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(DumbPtr a, DumbPtr b) noexcept {
            return std::compare_three_way{}(a.ptr_, b.ptr_);
        }

        /// @brief Compare against `nullptr` (covers both operand orders in C++20).
        [[nodiscard]] friend constexpr bool operator==(DumbPtr a, std::nullptr_t) noexcept {
            return a.ptr_ == nullptr;
        }

        /// @brief Compare against a raw `Pointer` without an explicit cast.
        [[nodiscard]] friend constexpr bool operator==(DumbPtr a, Pointer b) noexcept {
            return a.ptr_ == b;
        }
    };

    /// @brief Deduction guide: `DumbPtr(p)` deduces `DumbPtr<W>` from `W*`.
    template<class W>
    DumbPtr(W*) -> DumbPtr<W>;

    /// @brief Factory: build a `DumbPtr<W>` from `W*` with deduction.
    template<class W>
    [[nodiscard]] constexpr DumbPtr<W> MakeDumb(W* p) noexcept {
        return DumbPtr<W>{p};
    }

    /// @brief ADL `swap` for `DumbPtr` (exchanges observed pointers).
    template<class W>
    constexpr void swap(DumbPtr<W>& a, DumbPtr<W>& b) noexcept {
        a.Swap(b);
    }

    // Zero-overhead and triviality canaries (representative instantiations).
    static_assert(sizeof(DumbPtr<int>) == sizeof(int*));
    static_assert(alignof(DumbPtr<int>) == alignof(int*));
    static_assert(std::is_trivially_copyable_v<DumbPtr<int>>);
    static_assert(std::is_standard_layout_v<DumbPtr<int>>);
    static_assert(sizeof(DumbPtr<void>) == sizeof(void*));

    // -------------------------------------------------------------------------
    // Hash integration — free ADL hook. Hashes the address bits (identity), never the pointee. Found by 
    // Hashing::Hash via ADL on the DumbPtr argument; std::hash<DumbPtr<W>> is then auto-injected by Hash.h. 
    // Runtime-only: a pointer's numeric value is not a constant expression.
    // -------------------------------------------------------------------------

    /**
     * @brief Hash hook for `DumbPtr` — feeds the observed address to @p algo.
     *
     * @tparam Algo A Mashiro hash algorithm (constrained to expose `ResultType`, the property that distinguishes 
     *              it from arbitrary first args).
     * @tparam W    Observed type.
     * @return The algorithm's hash of the pointer's object representation.
     */
    template<class Algo, class W>
        requires requires { typename Algo::ResultType; }
    [[nodiscard]] Algo::ResultType HashValue(const Algo& algo, DumbPtr<W> p) noexcept {
        const auto raw = p.Get();
        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(raw)>>(raw);
        const std::span<const std::byte> data{bytes};
        if constexpr (requires(const Algo& a, std::span<const std::byte> s) { a(s); }) {
            return algo(data);
        } else {
            auto state = Algo::Seed();
            state.Feed(data);
            return state.Finalize();
        }
    }

    namespace Traits {

        /// @brief Concept: @p T is a specialisation of @ref Mashiro::DumbPtr.
        ///        Reflection-based, consistent with @ref StdOptional / @ref StdVariant.
        template<typename T>
        concept DumbPtrType = SpecializationOf<T, DumbPtr>;

        /// @brief The observed element type `W` of a @ref DumbPtrType.
        template<DumbPtrType T>
        using DumbPtrElement = typename std::remove_cvref_t<T>::ElementType;

    } // namespace Traits

} // namespace Mashiro
