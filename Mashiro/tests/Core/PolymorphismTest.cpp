/**
 * @file PolymorphismTest.cpp
 * @brief Tests for Core/Polymorphism.h — reflection-driven, inheritance-free runtime polymorphism.
 *
 * Coverage:
 * - Vtable layout shape (one slot per selected method, named after the source method).
 * - Thunk dispatch correctness (return value, argument forwarding, side effects).
 * - cv-qualifier preservation on the synthesised `this`-pointer.
 * - Method-selection policy: virtual / non-virtual / pure-virtual / Skip / Force annotations.
 * - `Implements` concept rejects structurally-incompatible Impls (signature, cv, return type).
 * - Adapter ergonomics — fat pointer semantics, value-copyable, vtable() returns the shared instance.
 *
 * Most checks are `STATIC_REQUIRE` against the synthesised vtable shape; behavioural correctness of
 * the thunks is exercised at runtime through fixture types whose methods record the call and assert
 * the values that flowed through.
 */
#include "Mashiro/Core/Polymorphism.h"
#include "Mashiro/Core/TypeTraits.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string>
#include <type_traits>

using namespace Mashiro;
namespace P = Mashiro::Polymorphism;

// =============================================================================
// Fixtures: a tiny "drawable" interface plus a structurally-matching, unrelated implementation.
// =============================================================================

namespace {

    struct IDrawable {
        virtual ~IDrawable() = default;
        virtual void Draw(int dx, int dy)  = 0;
        virtual int  Width() const         = 0;
        virtual void Resize(int newWidth)  = 0;
    };

    struct Square {                                  // Note: NO inheritance from IDrawable.
        int   width        = 0;
        int   lastDx       = 0;
        int   lastDy       = 0;
        int   drawCalls    = 0;
        int   resizeCalls  = 0;

        void Draw(int dx, int dy)        { lastDx = dx; lastDy = dy; ++drawCalls; }
        int  Width() const               { return width; }
        void Resize(int newWidth)        { width = newWidth; ++resizeCalls; }
    };

    // Wrong return type on Width — should fail Implements<>.
    struct BadReturn {
        void Draw(int, int)              {}
        unsigned Width() const           { return 0u; }
        void Resize(int)                 {}
    };

    // Wrong cv on Width (non-const where Iface requires const) — should fail Implements<>.
    struct BadCv {
        void Draw(int, int)              {}
        int  Width()                     { return 0; }
        void Resize(int)                 {}
    };

    // Missing Resize entirely — should fail Implements<>.
    struct Missing {
        void Draw(int, int)              {}
        int  Width() const               { return 0; }
    };

    // Annotation-driven selection fixture: only Skip-tagged methods are excluded; non-virtual gets
    // promoted by Force.
    struct IPolicy {
        virtual ~IPolicy() = default;
        virtual int  Compute() const                            = 0;     // selected (pure virtual)
        [[=P::Anno::Skip{}]]  virtual void DebugDump() const    {}       // explicitly skipped
        [[=P::Anno::Force{}]] int  HelperNotVirtual() const     { return 0; } // forced in
        void NonVirtualSilent() const                           {}       // not virtual, not forced -> excluded
    };

    struct PolicyImpl {
        int  Compute() const             { return 7; }
        int  HelperNotVirtual() const    { return 11; }
    };

} // namespace

// Complete each `Vtable<Iface, Impl>` aggregate at namespace scope, mirroring the SoA::Define
// precedent. Must run before any code queries the vtable's members or instantiates `kVtable`.
consteval { P::Define<IDrawable, Square>(); }
consteval { P::Define<IPolicy,   PolicyImpl>(); }
// Negative-fixture defines are intentionally absent: the `Implements` concept rejects these pairs
// before any `Define` call would be reached.

// =============================================================================
// Vtable shape — synthesised members
// =============================================================================

TEST_CASE("Vtable carries one named slot per selected interface method", AUTO_TAG) {
    using V = P::Vtable<IDrawable, Square>;
    STATIC_REQUIRE(Traits::MembersCount<V> == 3);

    constexpr auto names = Traits::MemberNames<V>;
    STATIC_REQUIRE(names[0] == "Draw");
    STATIC_REQUIRE(names[1] == "Width");
    STATIC_REQUIRE(names[2] == "Resize");
}

TEST_CASE("Vtable slots are typed as flat thunk pointers with cv-correct this", AUTO_TAG) {
    using V = P::Vtable<IDrawable, Square>;
    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 0>, void (*)(Square*, int, int)>);
    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 1>, int  (*)(const Square*)>);
    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 2>, void (*)(Square*, int)>);
}

// =============================================================================
// Implements<> concept — structural compatibility
// =============================================================================

TEST_CASE("Implements accepts a structurally-matching Impl", AUTO_TAG) {
    STATIC_REQUIRE(P::Implements<Square, IDrawable>);
}

TEST_CASE("Implements rejects mismatched return types, cv, and missing methods", AUTO_TAG) {
    STATIC_REQUIRE_FALSE(P::Implements<BadReturn, IDrawable>);
    STATIC_REQUIRE_FALSE(P::Implements<BadCv,     IDrawable>);
    STATIC_REQUIRE_FALSE(P::Implements<Missing,   IDrawable>);
}

// =============================================================================
// Annotation-driven selection
// =============================================================================

TEST_CASE("Anno::Skip and Anno::Force steer vtable composition", AUTO_TAG) {
    using V = P::Vtable<IPolicy, PolicyImpl>;
    // Compute (pure virtual, kept) + HelperNotVirtual (Force-promoted) = 2 slots.
    // DebugDump skipped, NonVirtualSilent excluded by default policy.
    STATIC_REQUIRE(Traits::MembersCount<V> == 2);

    constexpr auto names = Traits::MemberNames<V>;
    STATIC_REQUIRE(names[0] == "Compute");
    STATIC_REQUIRE(names[1] == "HelperNotVirtual");
}

// =============================================================================
// Behaviour: thunks dispatch into the implementation
// =============================================================================

TEST_CASE("Adapter dispatches calls and forwards arguments", AUTO_TAG) {
    Square s{};
    P::Adapter<IDrawable, Square> a{&s};

    a.vtable().Resize(a.target, 42);
    REQUIRE(s.width == 42);
    REQUIRE(s.resizeCalls == 1);

    a.vtable().Draw(a.target, -3, 7);
    REQUIRE(s.lastDx == -3);
    REQUIRE(s.lastDy == 7);
    REQUIRE(s.drawCalls == 1);

    REQUIRE(a.vtable().Width(a.target) == 42);
}

TEST_CASE("Adapter is a trivially-copyable fat pointer over the shared vtable", AUTO_TAG) {
    STATIC_REQUIRE(std::is_trivially_copyable_v<P::Adapter<IDrawable, Square>>);

    Square s{}, t{};
    P::Adapter<IDrawable, Square> a{&s};
    P::Adapter<IDrawable, Square> b = a;          // value-copy, shares the vtable instance.
    b.target = &t;

    a.vtable().Resize(a.target, 10);
    b.vtable().Resize(b.target, 20);
    REQUIRE(s.width == 10);
    REQUIRE(t.width == 20);
    REQUIRE(&a.vtable() == &b.vtable());          // single static instance.
    REQUIRE(&a.vtable() == &P::kVtable<IDrawable, Square>);
}

TEST_CASE("Annotation-selected vtable thunks dispatch into the unrelated impl", AUTO_TAG) {
    PolicyImpl impl{};
    P::Adapter<IPolicy, PolicyImpl> a{&impl};
    REQUIRE(a.vtable().Compute(a.target) == 7);
    REQUIRE(a.vtable().HelperNotVirtual(a.target) == 11);
}
