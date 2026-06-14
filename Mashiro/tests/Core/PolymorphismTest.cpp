/**
 * @file PolymorphismTest.cpp
 * @brief Tests for Core/Polymorphism.h — reflection-driven, inheritance-free runtime polymorphism.
 *
 * Coverage:
 * - Vtable layout shape (one slot per selected method, named after the source method).
 * - Thunk dispatch correctness: return value, argument forwarding, const/volatile this, references,
 *   rvalue parameters, return-by-reference, no-arg, and many-arg signatures.
 * - cv-qualifier preservation on the synthesised `this`-pointer.
 * - Method-selection policy: virtual / non-virtual / pure-virtual / Skip / Force annotations.
 * - `Implements` concept rejects structurally-incompatible Impls (signature, cv, return type, arity).
 * - Adapter ergonomics — fat pointer semantics, value-copyable, vtable() returns the shared instance,
 *   address stability, multiple distinct Impls projecting the same Iface.
 * - Stress: a wider interface, a non-trivial Impl with side effects, dispatch from const-Adapter.
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
#include <utility>

using namespace Mashiro;
namespace P = Mashiro::Polymorphism;

// =============================================================================
// Section 1 — Drawable: the canonical (Iface, Impl) pair used across most tests.
// =============================================================================

namespace {

    struct IDrawable {
        virtual ~IDrawable() = default;
        virtual void Draw(int dx, int dy) = 0;
        virtual int  Width() const        = 0;
        virtual void Resize(int newWidth) = 0;
    };

    struct Square {                         // Note: NO inheritance from IDrawable.
        int width       = 0;
        int lastDx      = 0;
        int lastDy      = 0;
        int drawCalls   = 0;
        int resizeCalls = 0;

        void Draw(int dx, int dy) { lastDx = dx; lastDy = dy; ++drawCalls; }
        int  Width() const        { return width; }
        void Resize(int n)        { width = n; ++resizeCalls; }
    };

    // A second, completely unrelated implementation projecting the same interface — exercises that
    // (Iface, Impl) is the parameterisation key, not just Iface.
    struct Circle {
        int radius        = 0;
        int drawHits      = 0;
        int lastTotalMove = 0;

        void Draw(int dx, int dy) { lastTotalMove = dx + dy; ++drawHits; }
        int  Width() const        { return 2 * radius; }
        void Resize(int n)        { radius = n / 2; }
    };

    // Wrong return type on Width — should fail Implements<>.
    struct BadReturn {
        void Draw(int, int)  {}
        unsigned Width() const { return 0u; }
        void Resize(int)     {}
    };

    // Wrong cv on Width (non-const where Iface requires const).
    struct BadCv {
        void Draw(int, int)  {}
        int  Width()         { return 0; }
        void Resize(int)     {}
    };

    // Missing Resize entirely.
    struct Missing {
        void Draw(int, int) {}
        int  Width() const  { return 0; }
    };

    // Extra arg on Draw — arity mismatch.
    struct BadArity {
        void Draw(int, int, int) {}
        int  Width() const       { return 0; }
        void Resize(int)         {}
    };

} // namespace

consteval { P::Define<IDrawable, Square>(); }
consteval { P::Define<IDrawable, Circle>(); }

// =============================================================================
// Section 2 — Vtable shape: layout, naming, slot typing, cv-correctness
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

TEST_CASE("Distinct Impls produce distinct, independent vtables for the same Iface", AUTO_TAG) {
    using VS = P::Vtable<IDrawable, Square>;
    using VC = P::Vtable<IDrawable, Circle>;

    STATIC_REQUIRE_FALSE(std::same_as<VS, VC>);
    STATIC_REQUIRE(std::same_as<Traits::MemberType<VC, 1>, int (*)(const Circle*)>);
    REQUIRE(reinterpret_cast<const void*>(&P::kVtable<IDrawable, Square>)
            != reinterpret_cast<const void*>(&P::kVtable<IDrawable, Circle>));
}

// =============================================================================
// Section 3 — Implements<> concept: structural acceptance / rejection
// =============================================================================

TEST_CASE("Implements accepts a structurally-matching Impl", AUTO_TAG) {
    STATIC_REQUIRE(P::Implements<Square, IDrawable>);
    STATIC_REQUIRE(P::Implements<Circle, IDrawable>);
}

TEST_CASE("Implements rejects every category of structural mismatch", AUTO_TAG) {
    STATIC_REQUIRE_FALSE(P::Implements<BadReturn, IDrawable>);   // wrong return type
    STATIC_REQUIRE_FALSE(P::Implements<BadCv,     IDrawable>);   // wrong cv on this
    STATIC_REQUIRE_FALSE(P::Implements<Missing,   IDrawable>);   // missing method
    STATIC_REQUIRE_FALSE(P::Implements<BadArity,  IDrawable>);   // arity mismatch
    STATIC_REQUIRE_FALSE(P::Implements<int,       IDrawable>);   // not a class
}

// =============================================================================
// Section 4 — Annotation-driven selection (Skip / Force / pure-virtual)
// =============================================================================

namespace {

    struct IPolicy {
        virtual ~IPolicy() = default;
        virtual int  Compute() const                            = 0;     // selected (pure virtual)
        [[=P::Anno::Skip{}]]  virtual void DebugDump() const    {}       // explicitly skipped
        [[=P::Anno::Force{}]] int  HelperNotVirtual() const     { return 0; } // forced in
        void NonVirtualSilent() const                           {}       // excluded by default
    };

    struct PolicyImpl {
        int Compute() const          { return 7; }
        int HelperNotVirtual() const { return 11; }
    };

} // namespace

consteval { P::Define<IPolicy, PolicyImpl>(); }

TEST_CASE("Anno::Skip and Anno::Force steer vtable composition", AUTO_TAG) {
    using V = P::Vtable<IPolicy, PolicyImpl>;
    STATIC_REQUIRE(Traits::MembersCount<V> == 2);

    constexpr auto names = Traits::MemberNames<V>;
    STATIC_REQUIRE(names[0] == "Compute");
    STATIC_REQUIRE(names[1] == "HelperNotVirtual");
}

TEST_CASE("Annotation-selected vtable thunks dispatch into the unrelated impl", AUTO_TAG) {
    PolicyImpl impl{};
    P::Adapter<IPolicy, PolicyImpl> a{&impl};
    REQUIRE(a.vtable().Compute(a.target) == 7);
    REQUIRE(a.vtable().HelperNotVirtual(a.target) == 11);
}

// =============================================================================
// Section 5 — Adapter ergonomics: copyability, vtable identity, address stability
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
    P::Adapter<IDrawable, Square> b = a;
    b.target = &t;

    a.vtable().Resize(a.target, 10);
    b.vtable().Resize(b.target, 20);
    REQUIRE(s.width == 10);
    REQUIRE(t.width == 20);
    REQUIRE(&a.vtable() == &b.vtable());
    REQUIRE(&a.vtable() == &P::kVtable<IDrawable, Square>);
}

TEST_CASE("vtable() is callable from a const Adapter and yields the same instance", AUTO_TAG) {
    Square s{};
    const P::Adapter<IDrawable, Square> a{&s};
    a.vtable().Resize(a.target, 5);
    REQUIRE(s.width == 5);
    REQUIRE(&a.vtable() == &P::kVtable<IDrawable, Square>);
}

TEST_CASE("Two distinct Impls of the same Iface dispatch independently", AUTO_TAG) {
    Square s{};
    Circle c{};

    P::Adapter<IDrawable, Square> as{&s};
    P::Adapter<IDrawable, Circle> ac{&c};

    as.vtable().Resize(as.target, 8);
    ac.vtable().Resize(ac.target, 10);
    REQUIRE(s.width == 8);
    REQUIRE(c.radius == 5);

    as.vtable().Draw(as.target, 1, 2);
    ac.vtable().Draw(ac.target, 3, 4);
    REQUIRE(s.drawCalls == 1);
    REQUIRE(c.drawHits == 1);
    REQUIRE(c.lastTotalMove == 7);

    REQUIRE(as.vtable().Width(as.target) == 8);
    REQUIRE(ac.vtable().Width(ac.target) == 10);
}

// =============================================================================
// Section 6 — Signature breadth: const, volatile, references, rvalue, return-by-ref, no-arg
// =============================================================================

namespace {

    struct Tracker {
        int  pure        = 0;
        int  withMut     = 0;
        bool seenConst   = false;
        bool seenVol     = false;
    };

    struct ICvSurface {
        virtual ~ICvSurface()                            = default;
        virtual void Mutating()                          = 0;     // mutable this
        virtual int  ReadOnly() const                    = 0;     // const this
        virtual int  Volatile() volatile                 = 0;     // volatile this
        virtual void Nullary()                           = 0;     // 0 args
        virtual int  Sum(int a, int b, int c, int d, int e, int f) = 0;        // many args
        virtual void TakeByRef(int& out)                 = 0;     // lvalue-ref param
        virtual void TakeByConstRef(const int& in)       = 0;     // const-ref param
        virtual void TakeByRvalueRef(std::string&& s)    = 0;     // rvalue-ref param (forwarded)
        virtual int& BackByLvalueRef()                   = 0;     // returns int&
    };

    struct CvImpl {
        Tracker     t{};
        std::string moved{};
        int         backing = 99;
        int         lastSum = 0;

        void Mutating()                              { ++t.withMut; }
        int  ReadOnly() const                        { return 7; }
        int  Volatile() volatile                     { return 13; }
        void Nullary()                               { ++t.pure; }
        int  Sum(int a, int b, int c, int d, int e, int f) {
            lastSum = a + b + c + d + e + f;
            return lastSum;
        }
        void TakeByRef(int& out)                     { out = 42; }
        void TakeByConstRef(const int& in)           { lastSum = in; }
        void TakeByRvalueRef(std::string&& s)        { moved = std::move(s); }
        int& BackByLvalueRef()                       { return backing; }
    };

} // namespace

consteval { P::Define<ICvSurface, CvImpl>(); }

TEST_CASE("Vtable preserves cv on this-pointer for const and volatile methods", AUTO_TAG) {
    using V = P::Vtable<ICvSurface, CvImpl>;
    constexpr auto names = Traits::MemberNames<V>;
    STATIC_REQUIRE(names[0] == "Mutating");
    STATIC_REQUIRE(names[1] == "ReadOnly");
    STATIC_REQUIRE(names[2] == "Volatile");

    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 0>, void (*)(CvImpl*)>);
    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 1>, int  (*)(const CvImpl*)>);
    STATIC_REQUIRE(std::same_as<Traits::MemberType<V, 2>, int  (*)(volatile CvImpl*)>);
}

TEST_CASE("Reference parameters propagate through the thunk", AUTO_TAG) {
    CvImpl impl{};
    P::Adapter<ICvSurface, CvImpl> a{&impl};

    int sink = 0;
    a.vtable().TakeByRef(a.target, sink);
    REQUIRE(sink == 42);                          // lvalue ref written through

    a.vtable().TakeByConstRef(a.target, 17);
    REQUIRE(impl.lastSum == 17);                  // const-ref read

    a.vtable().TakeByRvalueRef(a.target, std::string{"moved-in"});
    REQUIRE(impl.moved == "moved-in");            // rvalue-ref forwarded
}

TEST_CASE("Return-by-lvalue-reference is preserved through the thunk", AUTO_TAG) {
    CvImpl impl{};
    P::Adapter<ICvSurface, CvImpl> a{&impl};
    int& ref = a.vtable().BackByLvalueRef(a.target);
    REQUIRE(&ref == &impl.backing);
    ref = 31;
    REQUIRE(impl.backing == 31);
}

TEST_CASE("No-arg and many-arg signatures dispatch correctly", AUTO_TAG) {
    CvImpl impl{};
    P::Adapter<ICvSurface, CvImpl> a{&impl};

    a.vtable().Nullary(a.target);
    REQUIRE(impl.t.pure == 1);

    int r = a.vtable().Sum(a.target, 1, 2, 3, 4, 5, 6);
    REQUIRE(r == 21);
    REQUIRE(impl.lastSum == 21);
}

TEST_CASE("Volatile slot dispatches to a volatile-qualified Impl method", AUTO_TAG) {
    // The vtable slot has type `int (*)(volatile CvImpl*)`; the call site needs a
    // `volatile CvImpl*` argument, which is binding-compatible with `CvImpl*`.
    CvImpl impl{};
    P::Adapter<ICvSurface, CvImpl> a{&impl};
    volatile CvImpl* vp = a.target;
    REQUIRE(a.vtable().Volatile(vp) == 13);
}

// =============================================================================
// Section 7 — kVtable storage properties: addresses, identity, immutability
// =============================================================================

TEST_CASE("kVtable instances have static storage and stable addresses", AUTO_TAG) {
    const auto* p1 = &P::kVtable<IDrawable, Square>;
    const auto* p2 = &P::kVtable<IDrawable, Square>;
    REQUIRE(p1 == p2);                                    // single instance per (Iface, Impl).

    // Slot pointers are non-null for every selected method.
    REQUIRE(P::kVtable<IDrawable, Square>.Draw   != nullptr);
    REQUIRE(P::kVtable<IDrawable, Square>.Width  != nullptr);
    REQUIRE(P::kVtable<IDrawable, Square>.Resize != nullptr);
}

TEST_CASE("Distinct (Iface, Impl) instantiations install distinct thunks per slot", AUTO_TAG) {
    REQUIRE(reinterpret_cast<const void*>(P::kVtable<IDrawable, Square>.Draw)
            != reinterpret_cast<const void*>(P::kVtable<IDrawable, Circle>.Draw));
    REQUIRE(reinterpret_cast<const void*>(P::kVtable<IDrawable, Square>.Width)
            != reinterpret_cast<const void*>(P::kVtable<IDrawable, Circle>.Width));
}

// =============================================================================
// Section 8 — Polymorphism through a common interface: collection of unrelated impls
// =============================================================================

namespace {

    struct Triangle {
        int side          = 0;
        int totalDx       = 0;
        int totalDy       = 0;

        void Draw(int dx, int dy) { totalDx += dx; totalDy += dy; }
        int  Width() const        { return side; }
        void Resize(int n)        { side = n; }
    };

} // namespace

consteval { P::Define<IDrawable, Triangle>(); }

TEST_CASE("A scene of heterogeneous drawables dispatches uniformly through the IDrawable surface", AUTO_TAG) {
    // The classical use case: store distinct impls behind the same Iface adapter type-erased to a
    // common visit shape. Each adapter carries its own vtable pointer; the dispatch code is
    // signature-uniform.
    Square   s{};
    Circle   c{};
    Triangle t{};

    auto applyResize = [](auto& adapter, int w) {
        adapter.vtable().Resize(adapter.target, w);
    };
    auto applyDraw = [](auto& adapter, int dx, int dy) {
        adapter.vtable().Draw(adapter.target, dx, dy);
    };
    auto readWidth = [](auto& adapter) {
        return adapter.vtable().Width(adapter.target);
    };

    P::Adapter<IDrawable, Square>   as{&s};
    P::Adapter<IDrawable, Circle>   ac{&c};
    P::Adapter<IDrawable, Triangle> at{&t};

    applyResize(as, 4);
    applyResize(ac, 10);
    applyResize(at, 7);

    applyDraw(as, 1, 1);
    applyDraw(ac, 2, 3);
    applyDraw(at, 5, 6);
    applyDraw(at, -1, -2);

    REQUIRE(readWidth(as) == 4);
    REQUIRE(readWidth(ac) == 10);
    REQUIRE(readWidth(at) == 7);

    REQUIRE(s.drawCalls == 1);
    REQUIRE(c.drawHits == 1);
    REQUIRE(t.totalDx == 4);
    REQUIRE(t.totalDy == 4);
}

// =============================================================================
// Section 9 — Sanity: Adapter has a small, predictable footprint
// =============================================================================

TEST_CASE("Adapter occupies exactly one pointer", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(P::Adapter<IDrawable, Square>) == sizeof(void*));
    STATIC_REQUIRE(std::is_trivially_destructible_v<P::Adapter<IDrawable, Square>>);
    STATIC_REQUIRE(std::is_standard_layout_v<P::Adapter<IDrawable, Square>>);
}

// =============================================================================
// Section 10 — Virtual / overridden methods on the implementation side
// =============================================================================

namespace {

    // The "interface" side stays a plain duck-typed contract — no virtuals required on Iface for
    // selection by Anno::Force. (Anno::Force is the canonical way to project a non-inheritance
    // contract.)
    struct ICounter {
        virtual ~ICounter()     = default;
        virtual int Get() const = 0;
        virtual void Inc()      = 0;
    };

    // Impl declares the matching methods virtual so a derived class can override them. The vtable
    // captures a PMF to the *base* method; through the language's normal virtual-dispatch rules,
    // calling that PMF on a derived object reaches the most-derived override.
    struct VirtualBase {
        int n = 0;

        virtual int  Get() const { return n; }
        virtual void Inc()       { ++n; }
    };

    struct VirtualDerived : VirtualBase {
        int multiplier = 1;

        int  Get() const override { return n * multiplier; }
        void Inc() override       { n += multiplier; }
    };

    // Pure-virtual on Impl side: the base is abstract, but a concrete derived satisfies the slots.
    struct AbstractImpl {
        int seed = 0;
        virtual int  Get() const = 0;
        virtual void Inc()       = 0;
    };

    struct ConcreteFromAbstract : AbstractImpl {
        int  Get() const override { return seed * 2; }
        void Inc() override       { ++seed; }
    };

} // namespace

consteval { P::Define<ICounter, VirtualBase>(); }
consteval { P::Define<ICounter, AbstractImpl>(); }

TEST_CASE("Vtable bound to a virtual base method dispatches to the most-derived override", AUTO_TAG) {
    // Adapter is parameterised on VirtualBase, but the bound object is a VirtualDerived. The PMF
    // installed in the vtable points at VirtualBase::Get/Inc; those are virtual, so the PMF call
    // performs the runtime indirection through VirtualDerived's own vptr.
    VirtualDerived d{};
    d.n = 4;
    d.multiplier = 3;

    P::Adapter<ICounter, VirtualBase> a{&d};
    REQUIRE(a.vtable().Get(a.target) == 12);     // 4 * 3 = 12  -> derived override won.
    a.vtable().Inc(a.target);                    // n += multiplier (3) -> n == 7.
    REQUIRE(d.n == 7);
    REQUIRE(a.vtable().Get(a.target) == 21);     // 7 * 3 = 21.
}

TEST_CASE("Vtable bound to a pure-virtual Impl base dispatches via the concrete derived override", AUTO_TAG) {
    ConcreteFromAbstract c{};
    c.seed = 5;
    P::Adapter<ICounter, AbstractImpl> a{&c};
    REQUIRE(a.vtable().Get(a.target) == 10);     // 5 * 2 = 10.
    a.vtable().Inc(a.target);
    REQUIRE(c.seed == 6);
    REQUIRE(a.vtable().Get(a.target) == 12);
}

// =============================================================================
// Section 11 — Iface inheritance: which methods does Define<> see?
// =============================================================================

namespace {

    struct IBase {
        virtual ~IBase()                   = default;
        virtual int  BaseValue() const     = 0;
        virtual void Touch()               = 0;
    };

    struct IDerived : IBase {
        virtual int DerivedOnly() const    = 0;
        // Note: does NOT override BaseValue / Touch — they remain pure on IDerived.
    };

    struct DerivedImpl {
        int  v        = 0;
        int  touchHits = 0;

        int  BaseValue() const   { return v; }
        void Touch()             { ++touchHits; }
        int  DerivedOnly() const { return v + 100; }
    };

} // namespace

consteval { P::Define<IDerived, DerivedImpl>(); }

TEST_CASE("Iface inheritance: vtable only carries the derived interface's directly-declared methods", AUTO_TAG) {
    // Documented current behaviour: SelectMethods walks std::meta::members_of, which returns the
    // direct (declared-in-this-class) members only. Inherited pure-virtual methods from IBase are
    // therefore *not* projected into the vtable.
    //
    // This test pins the current behaviour so any future change (e.g. a recursive walk of
    // bases_of) trips a deliberate update of the test rather than a silent capability shift.
    using V = P::Vtable<IDerived, DerivedImpl>;
    STATIC_REQUIRE(Traits::MembersCount<V> == 1);
    STATIC_REQUIRE(Traits::MemberNames<V>[0] == "DerivedOnly");
}

TEST_CASE("Iface inheritance: dispatch reaches the derived-only method on the impl", AUTO_TAG) {
    DerivedImpl d{};
    d.v = 5;
    P::Adapter<IDerived, DerivedImpl> a{&d};
    REQUIRE(a.vtable().DerivedOnly(a.target) == 105);
}
