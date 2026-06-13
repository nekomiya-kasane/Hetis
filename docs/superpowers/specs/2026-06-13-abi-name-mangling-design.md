# ABI Name Mangling — Design Spec

**Date:** 2026-06-13
**Topic:** Reflection-driven compile-time name mangling (`Mashiro::ABI`)
**Status:** Approved design, pre-implementation
**File touched:** `Mashiro/include/Mashiro/Core/ABI.h`

## 1. Purpose & Success Criteria

Add a reflection-driven, compile-time name-mangling facility to Mashiro Core.
Given a `std::meta::info` (a P2996 reflection of a class, function, or
variable), compute its **Itanium ABI** and **MSVC ABI** mangled name entirely at
compile time (`consteval`), with a runtime wrapper to **demangle** names back to
human form. The mangler must exploit C++26 reflection, consteval blocks, and
expansion statements; the result is a static-storage compile-time constant with
zero runtime cost.

**Success criteria:**

- `ABI::Itanium::Mangle(^^T)` and `ABI::MSVC::Mangle(^^T)` produce correct
  mangled names for types, functions, and variables, including full
  substitution / back-reference compression.
- **MSVC output is verified byte-for-byte against the compiler** (the native
  target ABI), using `typeid(T).raw_name()`, `llvm-undname`, and `llvm-nm` on
  real `.obj` output as oracles.
- **Itanium output is verified against hand-derived golden strings** taken from
  the Itanium C++ ABI spec (the toolchain never emits Itanium symbols on this
  target — see §2).
- `ABI::Runtime::Demangle(...)` recovers the human signature at runtime.
- Comprehensive Catch2 tests, with each mangling sub-rule independently checked.

## 2. Toolchain & ABI Reality (decisive context)

The COCA `clang-p2996` toolchain's native target is **`x86_64-pc-windows-msvc`**.
clang uses a GNU-style driver (`-std=gnu++26 -freflection-latest`, libc++ via
`-isystem`) but **emits MSVC-decorated symbols** and links the MSVC/UCRT
sysroot. Consequences that shape this design:

- **MSVC is the byte-verified side.** The compiler is the oracle.
- **Itanium is implemented to spec** and checked against hand-written golden
  strings; the compiler cannot validate it on this target.
- The MSVC sysroot has **no `cxxabi.h`** (`__cxa_demangle`) and **no
  `dbghelp.h`** (`UnDecorateSymbolName`). The runtime demangler must bind its
  backend without relying on those headers being present (see §8).
- Oracles available in the toolchain `bin/`: `llvm-undname`, `llvm-cxxfilt`,
  `llvm-nm`, `llvm-readobj`, `llvm-objdump`.

## 3. Required C++26 / Reflection Features

All already probed mandatory in `cmake/ReflectionFeatureProbes.cmake`:

- **P2996** static reflection (`std::meta`, `^^`, splice `[:…:]`).
- **P3491** `std::define_static_string` / `define_static_array` — promotes the
  consteval-built mangled `std::string` to a static `std::string_view`.
- **P1306** expansion statements (`template for`) — iterate reflected template
  arguments and function parameters without index-sequence boilerplate.
- **P3289** consteval blocks — used where compile-time generation is cleaner.
- `__int128` / `__uint128_t` recognised as builtins (`n` / `o` in Itanium).

## 4. Scope

**In scope (entity classes):**

- **Types** — builtins; pointer / lvalue-ref / rvalue-ref / array / function /
  pointer-to-member; cv-qualifiers; class/struct/union/enum; nested scopes;
  template specialisations (template-args).
- **Functions** — free and member; parameter type lists; return type (encoded
  only for function templates, per Itanium); cv / ref qualifiers; `noexcept`;
  calling convention and access level (MSVC).
- **Variables** — global / namespace-scope and static member; scope qualifiers
  and cv.

**Phase-2 / optional (designed for, not in first cut):**

- Special symbols — vtable (`TV`), typeinfo (`TI`/`TS`), guard variable (`GV`),
  thunks. These bind tightly to compiler internals; deferred.

**Explicit non-goals:**

- Annotations do **not** influence mangled output. Mangling is purely
  reflection-driven (ABI fidelity is the whole point). "Leverage annotations"
  is satisfied by the reflection + consteval engine; no `[[=…]]` tag alters a
  mangled name. (An optional name-override hook is a documented future
  possibility, not part of this design.)
- No LLVM demangle-library dependency (per §8 decision).

## 5. Architecture

**Chosen approach: two independent consteval manglers behind a shared
reflection layer.** Itanium and MSVC each get a self-contained `consteval`
recursive descent that owns its own compression state. They share only a
`Detail` layer that answers *classification* questions about reflected entities
and never emits a mangling character.

Rationale: the two ABIs' compression schemes do not unify — Itanium uses an
unbounded base-36 substitution dictionary; MSVC uses two fixed 10-slot
back-reference tables. Forcing them through one visitor or one policy interface
would produce a leaky abstraction riddled with per-ABI branches in exactly the
compression code that is the #1 correctness risk. Keeping them separate lets
each be read, tested, and verified against its own oracle in isolation. The
genuinely shared part — *reflection mechanics* — is stateless and factored once.

### 5.1 Public API surface

Everything lives in the existing single header
`Mashiro/include/Mashiro/Core/ABI.h` (matching `Hash.h` / `TypeTraits.h`
style), under `Mashiro::ABI`:

```cpp
namespace Mashiro::ABI {

    enum class Kind { Itanium, MSVC };

    namespace Detail   { /* shared reflection classification (stateless) */ }
    namespace Itanium  { consteval std::string_view Mangle(std::meta::info); }
    namespace MSVC     { consteval std::string_view Mangle(std::meta::info); }

    // Generic entry — dispatches on Kind (NTTP so it stays consteval).
    template <Kind K>
    consteval std::string_view Mangle(std::meta::info entity);

    namespace Runtime {
        std::string Demangle(std::string_view mangled);               // runtime
        std::optional<std::string> TryDemangle(std::string_view mangled) noexcept;
    }
}
```

- **`Mangle` returns `std::string_view`** backed by `std::define_static_string`
  (P3491): the mangled name is a static-storage compile-time constant — zero
  runtime cost, matching the codebase's stated contract.
- **`Mangle` is `consteval`.** Mangling is a pure compile-time computation
  (the symbol already exists at link time). `consteval` guarantees folding and
  lets the implementation use `std::vector<std::meta::info>` freely for the
  substitution dictionary during evaluation.
- **`Demangle` is the only runtime function** — input may come from a linker
  map or crash dump; output is computed at runtime, hence `std::string`.
- The `template <Kind K> Mangle` wrapper lets callers write
  `ABI::Mangle<ABI::Kind::MSVC>(^^Foo)`.

### 5.2 Shared `Detail` reflection-classification layer

The only shared code. Zero mangling-format knowledge; each function `consteval`.

**Builtin classification (central technique).** Reflection `info` equality
compares the *type entity*, not its size. A builtin is classified by comparing
the dealiased, cv-stripped reflection against a fixed `^^`-reference table. This
distinguishes `long` from `int` (both 64-bit here) and `char` / `signed char` /
`unsigned char` as three distinct types — exactly what both ABIs require.

```cpp
enum class Builtin {
    Void, Bool, Char, SChar, UChar, Char8, Char16, Char32, WChar,
    Short, UShort, Int, UInt, Long, ULong, LongLong, ULongLong,
    Int128, UInt128, Float, Double, LongDouble, NullPtr, None
};
consteval Builtin ClassifyBuiltin(std::meta::info t);   // ^^int==t, ^^long==t, …
```

**Type-structure decomposition.** Thin wrappers over confirmed `std::meta`
predicates/transforms: `Strip(t)` (dealias + remove top-level cv, returning the
cv-flags separately); `IsPointer / IsLValueRef / IsRValueRef / IsArray /
IsFunction / IsMemberPointer`; pointee / element / referent recovery. Where
`std::meta` lacks a direct `remove_pointer`, reconstruct via documented
transforms; the implementation plan names the exact metafunction per case.

**Name & scope.** `ScopeChain(t)` reuses the existing `Traits::Detail::ScopeChain`
pattern (walk `parent_of` to global / anonymous), yielding innermost-first
reflections. `SourceName(info)` → `identifier_of`. The mangler **never** parses
`display_string_of` to drive output (it renders elaborated/alias forms);
`display_string_of` is used only for human labels in test diagnostics.

**Function decomposition.** `parameters_of`, `return_type_of`,
`has_ellipsis_parameter`, `is_noexcept`, member cv/ref qualifiers
(`is_const` / `is_volatile` / `is_*_reference_qualified`), `is_static_member`,
access level.

**Aliases.** Every entity entering a mangler is `dealias`'d first — a `typedef`
must mangle as its target.

Contract: this layer answers classification questions, is stateless, and emits
no characters. Both manglers consume it; neither can corrupt the other through
it.

## 6. Itanium Mangler (`ABI::Itanium`)

A `consteval` recursive descent that builds a `std::string` and threads a
substitution dictionary through the walk. This is where correctness lives, so it
is pinned exactly.

**Entry dispatch** on entity kind (`is_type` / `is_function` / `is_variable`):

- Type → bare `<type>` (no `_Z`), matching `typeid(T).name()` form.
- Function → `_Z <encoding>` = `<name> <bare-function-type>` (parameter types
  only; return type encoded only for function templates).
- Variable → `_Z <name>`.

**Substitution engine** (the #1 bug source — pinned exactly):

- State = `std::vector<std::meta::info>` of candidates in first-appearance
  order, compared by reflection `==`.
- `<substitution> ::= S <seq-id> _ | S_`; **seq-id is base-36** over `0-9A-Z`.
  First candidate → `S_`, second → `S0_`, third → `S1_`, … (i.e. `S_` = index 0,
  `S<n>_` = index n+1).
- **Candidates:** every non-builtin `<type>`; every `<template-template-arg>`;
  every `<prefix>` component (each enclosing namespace/class qualifier).
- **Not candidates:** builtin types; the standard-substitution abbreviations
  themselves (`St` / `Sa` / …); qualifiers fold into the type they qualify.
- Protocol: before emitting a component, scan the dictionary; on hit emit its
  `S`-ref and **do not** re-register; on miss emit the full encoding and append
  exactly one dictionary entry.

**Builtin table** (from `Detail::ClassifyBuiltin`):
`v`=void, `b`=bool, `c`=char, `a`=signed char, `h`=unsigned char,
`Ds`=char16_t, `Du`=char8_t, `Di`=char32_t, `w`=wchar_t, `s`=short,
`t`=unsigned short, `i`=int, `j`=unsigned int, `l`=long, `m`=unsigned long,
`x`=long long, `y`=unsigned long long, `n`=__int128, `o`=unsigned __int128,
`f`=float, `d`=double, `e`=long double, `Dn`=nullptr_t.

**Compounds:** CV order `r` (restrict) / `V` (volatile) / `K` (const);
`P` (ptr), `R` (lvalue-ref), `O` (rvalue-ref), `A<n>_` (array),
`M` (member-ptr), `F…E` (function type); nested names
`N [cv] [refq] <prefix> <unqualified-name> E`; source-names are length-prefixed
identifiers; template-ids `I <args> E`; standard substitutions
`St Sa Ss Sb Si So Sd`.

**Recursion uses `template for`** (P1306) over `template_arguments_of` and
`parameters_of`.

Each rule above is a separately golden-tested unit.

## 7. MSVC Mangler (`ABI::MSVC`)

A separate `consteval` descent with its **own** state — two fixed 10-slot
back-reference tables (one for name fragments `0-9`, one for compound types
`0-9`), matching clang's actual emitter, which is the spec of record here
(verified via `llvm-nm` on real `.obj`, `typeid(T).raw_name()`, and
`llvm-undname`).

- **Framing:** leading `?`, qualified name innermost-first each `@`-terminated,
  list closed by `@@`, then signature.
- **Primitive letters** (from `Detail::ClassifyBuiltin`):
  `X`=void, `D`=char, `C`=signed char, `E`=unsigned char, `F`=short,
  `G`=unsigned short, `H`=int, `I`=unsigned int, `J`=long, `K`=unsigned long,
  `_J`=long long, `_K`=unsigned long long, `M`=float, `N`=double,
  `O`=long double, `_N`=bool, `_W`=wchar_t, `_Q`=char8_t, `_S`=char16_t,
  `_U`=char32_t, `$$T`=nullptr_t.
- **Compounds (x64):** pointer `PEA`, ptr-to-const `PEB`, lvalue-ref `AEA`,
  rvalue-ref `$$QEA`; named types `?AV` (class) / `?AU` (struct) / `?AT` (union)
  / `?AW4` (enum).
- **Functions:** access+storage code (`Q` public member / `A` private /
  `I` protected / `Y` free / `S` static member), calling convention
  (`A`=__cdecl on x64), return-then-params, `X`-shortcut for a void parameter
  list, `@Z` terminator.
- **Variables:** data type + storage-class code (`A` normal, `B` const, …).

The two back-ref tables are the MSVC analogue of Itanium's substitution
dictionary; like there, they are the bug-prone core and get isolated unit tests.

## 8. Runtime Demangle (`ABI::Runtime`)

**Decision: conditionally-compiled native demangler, no LLVM library
dependency.** Selection is a compile-time `#if` on ABI/platform macros; exactly
one backend path compiles. No runtime branching, no dead backend linked.

```cpp
namespace Mashiro::ABI::Runtime {
    std::optional<std::string> TryDemangle(std::string_view mangled) noexcept;
    std::string Demangle(std::string_view mangled);  // input unchanged on failure
}
```

- **Native MSVC target** (`_WIN32` / MSVC ABI): call `UnDecorateSymbolName`
  from DbgHelp. Because the sysroot lacks `dbghelp.h` / `.lib`, **declare the
  prototype ourselves** and bind `dbghelp.dll` at runtime via `LoadLibraryW` +
  `GetProcAddress` (lazy, cached in a function-local `static` for thread-safe
  one-time init). DbgHelp is not thread-safe, so calls are serialised behind a
  private mutex.
- **Itanium target** (`__GNUC__` / Itanium ABI; only if a cross preset is ever
  added): declare and call `abi::__cxa_demangle`, freeing the returned buffer
  correctly.

`Mangle` stays `consteval` and never touches this. The compile-time mangle and
runtime demangle halves are fully independent units.

## 9. Testing

One Catch2 file `Mashiro/tests/Core/ABITest.cpp`, registered via
`add_mashiro_test(Core ABITest)`. Three tiers:

1. **Compile-time golden assertions (`STATIC_REQUIRE`)** — the primary
   correctness gate. Curated input types/functions/variables → expected mangled
   string, for **both** ABIs. MSVC strings are the compiler's real output
   (captured once via `llvm-nm` / `raw_name()`); Itanium strings are
   hand-derived from the ABI spec. Coverage matrix: each builtin; cv-qualifiers;
   pointer / ref / rvalue-ref / array; nested scopes; template-ids; `std::`
   standard substitutions; **substitution / back-ref compression edge cases**
   (repeated types, `PKc`, recursive structures); free vs member functions;
   variables.
2. **Self-consistency (`STATIC_REQUIRE` / `REQUIRE`)** — for every type `T` in
   the matrix, `MSVC::Mangle(^^T)` equals `typeid(T).raw_name()` (the compiler
   is the oracle; checked at compile time where usable, else runtime).
3. **Round-trip (runtime `REQUIRE`)** — `Runtime::Demangle(MSVC::Mangle(^^T))`
   contains the expected human signature, exercising the DbgHelp path.

Each mangling sub-rule (one builtin, one qualifier, one compression case) is its
own `TEST_CASE` so failures localise precisely.

## 10. References

- Itanium C++ ABI: <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling>
  (sections `#mangling`, `#mangle.type`, `#compression`, `#substitution`,
  `#builtin-type`).
- MSVC mangling (reverse-engineered): clang `lib/AST/MicrosoftMangle.cpp`
  (authoritative — clang is the producer here), LLVM
  `lib/Demangle/MicrosoftDemangle.cpp`, Wine `__unDName`, Wikiversity
  "Visual C++ name mangling".
- Existing project patterns: `Mashiro/include/Mashiro/Core/TypeTraits.h`
  (`ScopeChain`, `info` identity classification), `Hash.h` (annotation +
  `template for` over reflected members).
