# DumbPtr — Non-Owning Observer Pointer Design

**Date:** 2026-06-13
**Status:** Approved (pending spec review)
**Scope:** `Mashiro/include/Mashiro/Core/DumbPtr.h` + tests + CMake registration

## Problem

`Mashiro/Core/DumbPtr.h` exists but is incomplete and incorrect. It does not
compile (the `Traits::DumbPtrType` concept at line 58 is an unfinished
fragment), and several members are semantically wrong:

- `ConstPointer` is defined as `std::add_const_t<std::add_pointer_t<W>>` =
  `W* const` (a const *pointer*), when the intent reads as `const W*`.
- The templated copy constructor `DumbPtr(DumbPtr<W2> iPtr) : ptr_(iPtr)`
  initialises a `W*` from a `DumbPtr<W2>` — there is no implicit conversion,
  so this is ill-formed.
- `Reset(Pointer = nullptr)` ignores its argument and always stores `nullptr`.
- `Release() const` mutates `ptr_` through a const object.
- The const-qualified observers attempt deep-const propagation
  (`const W&` / `const W*`), which contradicts raw-pointer semantics.

The mixed signals (a `Release()` that nulls the pointer, suggesting
ownership transfer, but no destructor) indicate an unresolved design.

## Decision

`DumbPtr<W>` becomes the project's faithful equivalent of the proposed
`std::observer_ptr<W>`: a single `W*` that **documents intent** — "I observe,
I do not own." It never allocates, never frees, has no destructor side
effect, and is trivially copyable. It is the right abstraction for "a pointer
into something whose lifetime is owned elsewhere" (e.g. a back-reference, a
cached non-owning handle, a parent link).

Four semantic decisions (all confirmed with the requester):

1. **Ownership:** non-owning observer. No `Release()` ownership transfer, no
   destructor. (Differs slightly from `std::observer_ptr`, which keeps a
   `release()` that merely nulls + returns the raw pointer; omitted here per
   YAGNI and the explicit "non-owning" framing.)
2. **Framework integration:** deep — `DumbPtr` participates in the existing
   `Hashing::Hash`, `ToString`, and `ToJson` reflection frameworks.
3. **Hash / render / compare semantics:** by **identity (address)**, never by
   dereferencing the pointee. An observer is identified by *what it points
   at*, not by the pointee's contents. This never dereferences a possibly
   dangling/null pointer and never recurses on cyclic graphs.
4. **Const:** pointer-like, **no propagation**. `const DumbPtr<W>` still gives
   mutable `W&` / `W*` access — exactly like `W* const`. To observe-as-const,
   spell `DumbPtr<const W>`.

## Architecture

Primary header `DumbPtr.h`, plus a tiny optional bridge header
`DumbPtrJson.h`. Five layers, each independently understandable:

1. **The type** `DumbPtr<W>` — value semantics over a raw `W*`. (`DumbPtr.h`)
2. **Trait + factory** — `Traits::DumbPtrType` concept,
   `Traits::DumbPtrElement<T>`, `MakeDumb`, CTAD guide, free `swap`.
   (`DumbPtr.h`)
3. **Comparisons** — hidden-friend `==` / `<=>`, identity-based, total order.
   (`DumbPtr.h`)
4. **Hash + ToString ADL hooks** — `HashValue` and `ToString`, free functions
   in `namespace Mashiro` so ADL finds them. They need only standard-library
   types, so they are defined unconditionally and pull in **none** of the
   framework headers; the framework CPOs discover them via ADL when present.
   (`DumbPtr.h`)
5. **ToJson ADL hook** — `ToJson(DumbPtr<W>)`, in the bridge header
   `DumbPtrJson.h`. Unlike Hash/ToString, the ToJson hook must name the
   `nlohmann::json` return type, so it cannot be made include-free. The bridge
   header includes both `DumbPtr.h` and `ToJson.h` and defines the hook there,
   keeping the nlohmann dependency out of `DumbPtr.h`. (`DumbPtrJson.h`)

Dependency direction is preserved. `DumbPtr.h` includes only standard headers
(`<type_traits>`, `<compare>`, `<cstddef>`, `<cstdint>`, `<bit>`, `<array>`,
`<span>`, `<string>`, `<format>`) and `TypeTraits.h` (for
`Traits::SpecializationOf` and `Traits::TypeName`). It never includes
`Hash.h`, `ToString.h`, or `ToJson.h`. `std::hash<DumbPtr<W>>` is **not**
defined here — it is auto-injected by `Hash.h` for every `Hashable` type, so
it materialises for free in any TU that includes `Hash.h`. Only `DumbPtrJson.h`
carries the nlohmann dependency, and only callers wanting JSON include it.

## Component 1 — The type `DumbPtr<W>`

```cpp
template<class W>
class DumbPtr {
    W* ptr_ = nullptr;
public:
    using ElementType = W;
    using Pointer     = std::add_pointer_t<W>;        // W*

    // construction
    constexpr DumbPtr() noexcept = default;
    constexpr DumbPtr(std::nullptr_t) noexcept {}
    constexpr explicit DumbPtr(Pointer p) noexcept : ptr_(p) {}
    template<class W2> requires std::convertible_to<W2*, W*>   // derived→base, cv-correct
    constexpr DumbPtr(DumbPtr<W2> other) noexcept : ptr_(other.Get()) {}

    // observers — all const-qualified, all yield MUTABLE pointee access
    [[nodiscard]] constexpr Pointer Get()        const noexcept { return ptr_; }
    [[nodiscard]] constexpr Pointer operator->() const noexcept { return ptr_; }
    [[nodiscard]] constexpr std::add_lvalue_reference_t<W> operator*() const
        requires (!std::is_void_v<W>) { return *ptr_; }
    [[nodiscard]] constexpr explicit operator bool()    const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] constexpr explicit operator Pointer() const noexcept { return ptr_; }

    // modifiers — mutate the handle, not the pointee
    constexpr void Reset(Pointer p = nullptr) noexcept { ptr_ = p; }
    constexpr void Swap(DumbPtr& other) noexcept { auto t = ptr_; ptr_ = other.ptr_; other.ptr_ = t; }
};
```

Correctness invariants (locked by `static_assert` in the header):

- `sizeof(DumbPtr<W>) == sizeof(W*)` and `alignof` equal — zero overhead.
- `std::is_trivially_copyable_v` and `std::is_standard_layout_v` hold.
- **No const propagation:** every observer is `const`-qualified and returns
  `W&` / `W*` (not `const W&`). A `const DumbPtr<W>` behaves as `W* const`.
- **`DumbPtr<void>` is well-formed** as an opaque handle: `operator*` is
  constrained away via `requires (!std::is_void_v<W>)`; `Get`/`bool`/`Reset`/
  `Swap` remain usable.
- The templated converting constructor uses `std::convertible_to<W2*, W*>`,
  which admits derived→base upcasts and `T*`→`const T*`, and rejects
  unrelated or narrowing conversions — matching raw-pointer rules.

`Release()` is intentionally absent: a non-owner has no ownership to release.

## Component 2 — Trait, factory, CTAD, swap

```cpp
namespace Traits {
    /// Reflection-based probe, consistent with StdOptional / StdVariant.
    template<typename T>
    concept DumbPtrType = SpecializationOf<T, DumbPtr>;

    /// The observed element type W of a DumbPtr specialisation.
    template<DumbPtrType T>
    using DumbPtrElement = typename std::remove_cvref_t<T>::ElementType;
}

template<class W> DumbPtr(W*) -> DumbPtr<W>;                    // CTAD
template<class W> [[nodiscard]] constexpr DumbPtr<W> MakeDumb(W* p) noexcept { return DumbPtr<W>{p}; }
template<class W> constexpr void swap(DumbPtr<W>& a, DumbPtr<W>& b) noexcept { a.Swap(b); }
```

## Component 3 — Comparisons (identity / address based)

All as hidden friends (ADL-found, no namespace pollution), `constexpr noexcept`:

- `operator==` homogeneous (`DumbPtr<W>` vs `DumbPtr<W>`).
- `operator==` heterogeneous (`DumbPtr<W1>` vs `DumbPtr<W2>`), enabled only
  when `W1*` and `W2*` are equality-comparable.
- `operator==` vs `std::nullptr_t`.
- `operator==` vs raw `Pointer` (so users need not cast through the explicit
  `operator Pointer`).
- `operator<=>` via `std::compare_three_way` on the raw addresses, yielding
  `std::strong_ordering`. Using `compare_three_way` (not raw `<`) is the
  well-defined route to a total order over pointers, making `DumbPtr` a valid
  `std::set` / `std::map` key and `std::ranges::sort` element.

## Component 4 — Framework integration via self-contained ADL hooks

All three frameworks dispatch to an ADL free-function hook *before* their
generic branches (`Hashing::HashCPO` tries `HashValue(algo, v)`;
`ToStringImpl` tries `ToString(v)`; `ToJsonImpl` tries `ToJson(v)`). Declaring
those hooks in `namespace Mashiro` next to `DumbPtr` makes `DumbPtr` a
first-class citizen of every framework **with zero edits to the framework
headers** and **without `DumbPtr.h` including them**. A `DumbPtr` member nested
in any reflected struct routes through these hooks automatically.

Each hook is `requires`-guarded on a property of the framework's own types, so
it is only a viable overload when that framework is already in scope.

### Hash — `HashValue(const Algo&, DumbPtr<W>)`

```cpp
template<class Algo, class W>
    requires requires { typename Algo::ResultType; }   // an Algo from Hash.h
[[nodiscard]] constexpr typename Algo::ResultType
HashValue(const Algo& algo, DumbPtr<W> p) noexcept;
```

Hashes the **address bits** (the pointer `bit_cast` to a byte array fed to the
algorithm), never the pointee. Two observers of the same object hash equal;
distinct objects (almost always) hash distinctly. `std::hash<DumbPtr<W>>` then
comes for free via Hash.h's existing auto-injection, because `DumbPtr` becomes
`Hashable`. Address hashing is runtime-only (a pointer's numeric value is not a
constant expression), which matches how raw addresses behave everywhere.

### ToString — ADL `ToString(DumbPtr<W>)`

```cpp
template<class W>
[[nodiscard]] inline std::string ToString(DumbPtr<W> p);
// "null"  when empty, else  "DumbPtr<W>(0xADDR)"  using Traits::TypeName<W>
```

Renders identity, never dereferences. Mirrors the existing raw-pointer branch's
style. No `FromString`: an address parsed from text is meaningless and unsafe
for an observer, so the inverse is deliberately omitted.

### ToJson — ADL `ToJson(DumbPtr<W>)`, one-way (in `DumbPtrJson.h`)

```cpp
// in DumbPtrJson.h, which includes DumbPtr.h and ToJson.h
template<class W>
[[nodiscard]] inline json ToJson(DumbPtr<W> p);
// null when empty, else the address as a JSON value (diagnostic only)
```

Identity semantics mean the address is a *debug artifact*, not round-trippable
data — the pointer is invalid in any other process or run. So `ToJson` is
**one-way** (emit for logging/inspection); there is deliberately **no
`FromJson`**. A `DumbPtr` member in a serialised struct is therefore
output-only, which is the truthful model for a non-owning observer. Because the
hook must name the `nlohmann::json` return type, it lives in the bridge header
`DumbPtrJson.h` rather than `DumbPtr.h`, so the nlohmann dependency reaches only
callers who opt in by including the bridge.

## Testing

New target `Mashiro/tests/Core/DumbPtrTest.cpp`, registered in
`Mashiro/tests/CMakeLists.txt` as `add_mashiro_test(Core DumbPtrTest)` — the
exact existing pattern, Catch2-discovered, auto-tagged `[Core.DumbPtr]` via
`Support/Meta.h`. Test groups mirror `HashTest.cpp`:

1. **Compile-time invariants** (`STATIC_REQUIRE` / `static_assert`):
   `sizeof(DumbPtr<W>) == sizeof(W*)`, trivially-copyable, standard-layout;
   `Traits::DumbPtrType<DumbPtr<int>>` holds, `Traits::DumbPtrType<int*>`
   fails; `DumbPtr<void>` well-formed and its `operator*` absent (a `requires`
   probe); `DumbPtrElement<DumbPtr<Foo>>` is `Foo`.
2. **Construction / conversion:** default → null; `nullptr`; explicit from
   pointer; derived → base upcast (small `Base`/`Derived` pair);
   `T*` → `const T*`; CTAD; `MakeDumb`.
3. **Observers & const:** `Get` / `operator*` / `operator->` / `bool`; the key
   no-propagation test — `const DumbPtr<W>` yields a mutable `W&`
   (`static_assert(std::is_same_v<decltype(*cdp), W&>)`).
4. **Modifiers:** `Reset(p)` stores `p` (regression guard for the current
   bug); `Reset()` nulls; `Swap` and free `swap`.
5. **Comparisons / ordering:** `==` / `<=>` homogeneous, heterogeneous, vs
   `nullptr_t`, vs raw `Pointer`; `strong_ordering`; usable as a `std::set` /
   `std::map` key.
6. **`constexpr` proof:** a `consteval` block constructing, observing, and
   comparing `DumbPtr` over objects with static storage duration, asserted at
   compile time — mirrors the project's compile-time-folding requirement.
   (Address *hashing* is exercised at runtime, since numeric addresses are not
   constant expressions.)
7. **Framework integration** (each block `#include`s the relevant framework;
   the ToJson block includes `DumbPtrJson.h`):
   `std::hash<DumbPtr>` and `Hash()` produce stable address-based values, two
   observers of one object hash equal; `ToString` yields `"null"` /
   `"DumbPtr<…>(0x…)"`; `ToJson` emits `null` for empty and non-null
   otherwise; a reflected struct *containing* a `DumbPtr` member routes through
   Hash / ToString and emits through ToJson, proving nested-member routing.

## Out of Scope

- Ownership / RAII deletion (use `std::unique_ptr` / `std::shared_ptr`).
- `Release()` ownership transfer.
- Deep-const propagation (`DumbPtr<const W>` covers the const-observation
  need).
- `FromString` / `FromJson` for `DumbPtr` (addresses do not round-trip).
- Atomic or thread-safe variants.

## Success Criteria

- `DumbPtr.h` compiles cleanly under the project's C++26 toolchain with full
  Doxygen coverage matching the `Int128.h` / `TypeTraits.h` house style.
- All `DumbPtrTest` cases pass; the compile-time invariants hold as
  `static_assert`s.
- Zero runtime overhead vs a raw `W*` (size, alignment, codegen).
- No new coupling: `DumbPtr.h` includes none of `Hash.h` / `ToString.h` /
  `ToJson.h` (and no nlohmann), yet integrates with Hash and ToString when
  they are present. JSON integration is opt-in via the bridge header
  `DumbPtrJson.h`, which is the only place the nlohmann dependency appears.
