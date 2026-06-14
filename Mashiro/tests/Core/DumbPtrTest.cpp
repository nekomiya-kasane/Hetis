/**
 * @file DumbPtrTest.cpp
 * @brief Comprehensive tests for Mashiro::DumbPtr — the non-owning observer.
 *
 * Covers: compile-time invariants (size/triviality/concept), construction and
 * conversion, observers and the no-const-propagation rule, modifiers,
 * identity-based comparisons and total ordering, constexpr folding, and the
 * Hash / ToString / ToJson framework integrations (identity semantics).
 */
#include "Mashiro/Core/DumbPtr.h"

#include "Mashiro/Core/Hash.h"
#include "Mashiro/Core/ToString.h"
#include "Mashiro/Core/DumbPtrJson.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <type_traits>

using namespace Mashiro;

// =============================================================================
// Test types
// =============================================================================

namespace {

    struct Base {
        int b = 1;
        virtual ~Base() = default;
    };

    struct Derived : Base {
        int d = 2;
    };

    struct Plain {
        int x = 7;
        int y = 9;
    };

    // A reflected struct that *contains* a DumbPtr member, to prove nested
    // member routing through the frameworks.
    struct Holder {
        int tag = 3;
        DumbPtr<Plain> observed;
    };

} // anonymous namespace

// Detection concepts: hoisted into a dependent context so an absent (constrained-away)
// operator is a *soft* substitution failure rather than a hard error. With a concrete
// alias the clang-p2996 front-end evaluates the requires-body eagerly and reports the
// missing `operator*` / `operator->` as a fatal diagnostic instead of `false`.
namespace {
    template<class T>
    concept HasStarDeref = requires(T v) { *v; };
    template<class T>
    concept HasArrowDeref = requires(T v) { v.operator->(); };
} // anonymous namespace

// =============================================================================
// [Compile-time] — size, triviality, concept, element trait
// =============================================================================

TEST_CASE("Layout: DumbPtr is a zero-overhead pointer wrapper", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(DumbPtr<Plain>) == sizeof(Plain*));
    STATIC_REQUIRE(alignof(DumbPtr<Plain>) == alignof(Plain*));
    STATIC_REQUIRE(std::is_trivially_copyable_v<DumbPtr<Plain>>);
    STATIC_REQUIRE(std::is_standard_layout_v<DumbPtr<Plain>>);
    STATIC_REQUIRE(sizeof(DumbPtr<void>) == sizeof(void*));
}

TEST_CASE("Concept: DumbPtrType detects specialisations only", AUTO_TAG) {
    STATIC_REQUIRE(Traits::DumbPtrType<DumbPtr<int>>);
    STATIC_REQUIRE(Traits::DumbPtrType<DumbPtr<const Plain>>);
    STATIC_REQUIRE(!Traits::DumbPtrType<int*>);
    STATIC_REQUIRE(!Traits::DumbPtrType<int>);
    STATIC_REQUIRE(!Traits::DumbPtrType<Plain>);
}

TEST_CASE("Trait: DumbPtrElement recovers the observed type", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<Traits::DumbPtrElement<DumbPtr<Plain>>, Plain>);
    STATIC_REQUIRE(std::is_same_v<Traits::DumbPtrElement<DumbPtr<const int>>, const int>);
    STATIC_REQUIRE(std::is_same_v<Traits::DumbPtrElement<DumbPtr<void>>, void>);
}

TEST_CASE("Void: opaque handle has no deref operators", AUTO_TAG) {
    using V = DumbPtr<void>;
    STATIC_REQUIRE(!HasStarDeref<V>);
    STATIC_REQUIRE(!HasArrowDeref<V>);
    // but the handle ops still work
    STATIC_REQUIRE(requires(V v) { v.Get(); });
    STATIC_REQUIRE(requires(V v) { static_cast<bool>(v); });
}

// =============================================================================
// [Construction] — defaults, nullptr, explicit, upcast, cv, CTAD, factory
// =============================================================================

TEST_CASE("Ctor: default and nullptr produce an empty observer", AUTO_TAG) {
    DumbPtr<Plain> a;
    DumbPtr<Plain> b{nullptr};
    REQUIRE(a.Get() == nullptr);
    REQUIRE_FALSE(static_cast<bool>(a));
    REQUIRE(b.Get() == nullptr);
    REQUIRE(a == b);
    REQUIRE(a == nullptr);
}

TEST_CASE("Ctor: explicit from pointer observes the object", AUTO_TAG) {
    Plain p;
    DumbPtr<Plain> dp{&p};
    REQUIRE(dp.Get() == &p);
    REQUIRE(static_cast<bool>(dp));
    // Construction from a raw pointer is explicit (no implicit conversion).
    STATIC_REQUIRE(!std::is_convertible_v<Plain*, DumbPtr<Plain>>);
    STATIC_REQUIRE(std::is_constructible_v<DumbPtr<Plain>, Plain*>);
}

TEST_CASE("Ctor: derived to base upcast is allowed", AUTO_TAG) {
    Derived d;
    DumbPtr<Derived> dd{&d};
    DumbPtr<Base> db = dd;  // implicit upcast, like Derived* -> Base*
    REQUIRE(db.Get() == static_cast<Base*>(&d));
    REQUIRE(db->b == 1);
    // Unrelated conversion is rejected.
    STATIC_REQUIRE(!std::is_constructible_v<DumbPtr<Plain>, DumbPtr<Base>>);
}

TEST_CASE("Ctor: non-const to const observation is allowed", AUTO_TAG) {
    Plain p;
    DumbPtr<Plain> dp{&p};
    DumbPtr<const Plain> cdp = dp;  // Plain* -> const Plain*
    REQUIRE(cdp.Get() == &p);
    STATIC_REQUIRE(!std::is_constructible_v<DumbPtr<Plain>, DumbPtr<const Plain>>);
}

TEST_CASE("Factory: CTAD and MakeDumb deduce the element type", AUTO_TAG) {
    Plain p;
    DumbPtr dp{&p};  // CTAD
    STATIC_REQUIRE(std::is_same_v<decltype(dp), DumbPtr<Plain>>);
    auto md = MakeDumb(&p);
    STATIC_REQUIRE(std::is_same_v<decltype(md), DumbPtr<Plain>>);
    REQUIRE(dp == md);
}

// =============================================================================
// [Observers] — access and the no-const-propagation rule
// =============================================================================

TEST_CASE("Observers: Get / deref / arrow / bool", AUTO_TAG) {
    Plain p{10, 20};
    DumbPtr<Plain> dp{&p};
    REQUIRE(dp.Get() == &p);
    REQUIRE((*dp).x == 10);
    REQUIRE(dp->y == 20);
    REQUIRE(static_cast<bool>(dp));
    REQUIRE(static_cast<Plain*>(dp) == &p);
}

TEST_CASE("Const: handle constness does NOT propagate to the pointee", AUTO_TAG) {
    Plain p{1, 2};
    const DumbPtr<Plain> cdp{&p};  // const handle
    // A const DumbPtr<Plain> still yields mutable Plain& / Plain* (like Plain* const).
    STATIC_REQUIRE(std::is_same_v<decltype(*cdp), Plain&>);
    STATIC_REQUIRE(std::is_same_v<decltype(cdp.Get()), Plain*>);
    STATIC_REQUIRE(std::is_same_v<decltype(cdp.operator->()), Plain*>);
    cdp->x = 99;  // compiles and mutates through a const handle
    REQUIRE(p.x == 99);
    // Deep-const observation is opt-in via DumbPtr<const Plain>.
    STATIC_REQUIRE(std::is_same_v<decltype(*std::declval<DumbPtr<const Plain>>()), const Plain&>);
}

// =============================================================================
// [Modifiers] — Reset stores its argument (regression guard), Swap
// =============================================================================

TEST_CASE("Reset: stores the supplied pointer, then nulls", AUTO_TAG) {
    Plain p{5, 6};
    DumbPtr<Plain> dp;
    dp.Reset(&p);              // must STORE &p (old code ignored the arg)
    REQUIRE(dp.Get() == &p);
    dp.Reset();                // default nulls
    REQUIRE(dp.Get() == nullptr);
}

TEST_CASE("Swap: member and free swap exchange observed pointers", AUTO_TAG) {
    Plain p, q;
    DumbPtr<Plain> a{&p};
    DumbPtr<Plain> b{&q};
    a.Swap(b);
    REQUIRE(a.Get() == &q);
    REQUIRE(b.Get() == &p);
    swap(a, b);  // ADL free swap
    REQUIRE(a.Get() == &p);
    REQUIRE(b.Get() == &q);
}

// =============================================================================
// [Comparisons] — identity-based equality and total ordering
// =============================================================================

TEST_CASE("Compare: equality by observed identity", AUTO_TAG) {
    Plain p, q;
    DumbPtr<Plain> a{&p};
    DumbPtr<Plain> b{&p};
    DumbPtr<Plain> c{&q};
    REQUIRE(a == b);        // same object
    REQUIRE(a != c);        // different object
    REQUIRE(a == &p);       // vs raw pointer, no cast needed
    REQUIRE_FALSE(a == nullptr);
    DumbPtr<Plain> e;
    REQUIRE(e == nullptr);
}

TEST_CASE("Compare: strong_ordering yields a usable total order", AUTO_TAG) {
    Plain arr[3];
    DumbPtr<Plain> a{&arr[0]};
    DumbPtr<Plain> b{&arr[1]};
    STATIC_REQUIRE(std::is_same_v<decltype(a <=> b), std::strong_ordering>);
    // Usable as ordered-container keys (exercises operator<=> / operator<).
    std::set<DumbPtr<Plain>> s{a, b, a};
    REQUIRE(s.size() == 2);
    std::map<DumbPtr<Plain>, int> m;
    m[a] = 1;
    m[b] = 2;
    REQUIRE(m.at(a) == 1);
    REQUIRE(m.at(b) == 2);
}

// =============================================================================
// [constexpr] — construct / observe / compare fold at compile time
// =============================================================================

namespace {
    constexpr Plain kStaticPlain{42, 43};

    consteval bool ConstexprRoundTrip() {
        DumbPtr<Plain> dp{const_cast<Plain*>(&kStaticPlain)};
        if (!dp) return false;
        if (dp->x != 42) return false;
        if ((*dp).y != 43) return false;
        DumbPtr<Plain> copy = dp;
        if (copy != dp) return false;
        copy.Reset();
        if (copy != nullptr) return false;
        dp.Swap(copy);
        return dp == nullptr && copy.Get() == &kStaticPlain;
    }
}

TEST_CASE("Constexpr: full lifecycle folds at compile time", AUTO_TAG) {
    STATIC_REQUIRE(ConstexprRoundTrip());
}

// =============================================================================
// [Integration] — Hash / ToString / ToJson (identity semantics)
// =============================================================================

TEST_CASE("Hash: address-based, stable, equal for equal observers", AUTO_TAG) {
    Plain p, q;
    DumbPtr<Plain> a{&p};
    DumbPtr<Plain> b{&p};
    DumbPtr<Plain> c{&q};
    REQUIRE(Hashing::Hash(a) == Hashing::Hash(b));   // same object -> equal hash
    REQUIRE(Hashing::Hash(a) == Hashing::Hash(a));   // stable
    REQUIRE(Hashing::Hash(a) != Hashing::Hash(c));   // distinct objects (overwhelmingly)
    // std::hash auto-injected for the Hashable DumbPtr.
    std::hash<DumbPtr<Plain>> h;
    REQUIRE(h(a) == h(b));
}

TEST_CASE("ToString: renders identity, never dereferences", AUTO_TAG) {
    DumbPtr<Plain> empty;
    REQUIRE(ToString(empty) == "null");

    Plain p;
    DumbPtr<Plain> dp{&p};
    auto s = ToString(dp);
    REQUIRE(s.starts_with("DumbPtr<"));
    REQUIRE(s.find("0x") != std::string::npos);
}

TEST_CASE("ToJson: one-way emit, null for empty observer", AUTO_TAG) {
    DumbPtr<Plain> empty;
    REQUIRE(ToJson(empty).is_null());

    Plain p;
    DumbPtr<Plain> dp{&p};
    auto j = ToJson(dp);
    REQUIRE(j.is_string());
    REQUIRE(j.get<std::string>().starts_with("0x"));
}

TEST_CASE("Nested: a DumbPtr member routes through the frameworks", AUTO_TAG) {
    Plain p{1, 2};
    Holder h{.tag = 3, .observed = DumbPtr<Plain>{&p}};
    Holder h2{.tag = 3, .observed = DumbPtr<Plain>{&p}};

    // Hash of the aggregate folds in the member's address-based hash.
    REQUIRE(Hashing::Hash(h) == Hashing::Hash(h2));

    // ToString of the aggregate includes the member's identity rendering.
    auto s = ToString(h);
    REQUIRE(s.find("DumbPtr<") != std::string::npos);

    // ToJson of the aggregate emits the member as a non-null address string.
    auto j = ToJson(h);
    REQUIRE(j.is_object());
    REQUIRE(j.contains("observed"));
    REQUIRE(j["observed"].is_string());
}
