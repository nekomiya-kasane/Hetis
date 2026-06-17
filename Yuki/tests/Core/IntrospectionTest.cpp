/**
 * @file IntrospectionTest.cpp
 * @brief Tests for the metaclass-level introspection surface declared in
 *        @ref Yuki/Core/Introspection.h (Task 14).
 *
 * Spec refs: §1.2 (RT operations); §6.1 (potential attributes); §6.3 (potential-vs-runtime
 * symmetry); §6.6 (lifetime of views). Each test installs a fresh (Implementation, Extension)
 * pair, then queries the metaclass through the Introspection surface — no closure instance is
 * touched. Fixtures are T14-suffixed so Catch2's randomiser cannot interleave them with the
 * same-conceptual fixtures from other test binaries.
 */
#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>

using namespace Yuki;

namespace {

    // Two interfaces, two roles for the closure under test:
    // - IAT14 is the "BOA" interface — AImplT14 truly C++-inherits it, so the dispatch entry
    //   for IAT14 is a DirectCast and the provider is AImplT14 itself.
    // - IBT14 is the "extension-only" interface — AImplT14 does NOT inherit it; the BExtT14
    //   Extension below adds it via Implements, taking the SideTableResolver arm because it has
    //   one NSDM and is therefore stateful.
    struct [[=Anno::Interface]] IAT14 {
        virtual ~IAT14() = default;
    };
    struct [[=Anno::Interface]] IBT14 {
        virtual ~IBT14() = default;
    };

    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IAT14}]]
           AImplT14 : MetaNode<AImplT14>, IAT14 {};

    // Stateful (one NSDM) + lazy (no Anno::Eager) => SideTableResolver arm. The Extension
    // advertises IBT14 via Implements; a Query<IBT14>(&aimpl) would hit the resolver and
    // materialise the facade on demand — but the introspection surface answers without ever
    // invoking the resolver. The constructor-takes-Extendee* ctor comes from ExtensionNode.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^AImplT14}]] [[=Anno::Implements{^^IBT14}]]
           BExtT14 : ExtensionNode<BExtT14, AImplT14> {
        using ExtensionNode::ExtensionNode;
        int n = 0;
    };

    // Eager fixture (stateful + Anno::Eager) — exercises EagerExtensions(). Distinct from the
    // lazy fixture above so we can install both in one binary without one masking the other's
    // observable shape on the metaclass.
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IAT14}]]
           AImplEagerT14 : MetaNode<AImplEagerT14>, IAT14 {};
    struct [[=Anno::Extension]] [[=Anno::Extends{^^AImplEagerT14}]]
           [[=Anno::Implements{^^IBT14}]] [[=Anno::Eager{}]]
           BExtEagerT14 : ExtensionNode<BExtEagerT14, AImplEagerT14> {
        using ExtensionNode::ExtensionNode;
        int n = 0;
    };

} // namespace

// =============================================================================
// IidsOf / Capabilities — the union of every interface iid the closure resolves
// =============================================================================

TEST_CASE("RT::IidsOf lists every iid the closure can resolve", AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    auto& m = MetaClassOf<AImplT14>;
    auto iids = RT::IidsOf(m);

    // Both the BOA-inherited interface and the Extension-supplied one must appear after both
    // Installs. The transform_view's iterator over a span<const DispatchEntry> is a
    // contiguous-iterator wrapper, so std::ranges::find works without surprises.
    REQUIRE(std::ranges::find(iids, IidOf<IAT14>()) != std::ranges::end(iids));
    REQUIRE(std::ranges::find(iids, IidOf<IBT14>()) != std::ranges::end(iids));
}

TEST_CASE("RT::Capabilities is a synonym for RT::IidsOf", AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    auto& m = MetaClassOf<AImplT14>;
    auto caps = RT::Capabilities(m);

    REQUIRE(std::ranges::find(caps, IidOf<IAT14>()) != std::ranges::end(caps));
    REQUIRE(std::ranges::find(caps, IidOf<IBT14>()) != std::ranges::end(caps));
}

// =============================================================================
// Provides<I> + ProviderClass<I> — agreement and per-arm provider attribution
// =============================================================================

TEST_CASE("RT::Provides<I> and RT::ProviderClass<I> agree on per-arm attribution", AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    auto& m = MetaClassOf<AImplT14>;
    REQUIRE(RT::Provides<IAT14>(m));
    REQUIRE(RT::Provides<IBT14>(m));

    // BOA arm: the Implementation is its own provider for the inherited interface.
    REQUIRE(RT::ProviderClass<IAT14>(m) == &MetaClassOf<AImplT14>);
    // Extension override: the Extension owns the IBT14 dispatch entry it just published.
    REQUIRE(RT::ProviderClass<IBT14>(m) == &MetaClassOf<BExtT14>);
}

// =============================================================================
// ProviderDispatchKind<I> — surfaces the resolved arm without driving dispatch
// =============================================================================

TEST_CASE("RT::ProviderDispatchKind reports the resolved arm", AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    auto& m = MetaClassOf<AImplT14>;
    REQUIRE(RT::ProviderDispatchKind<IAT14>(m) == DispatchKind::DirectCast);
    REQUIRE(RT::ProviderDispatchKind<IBT14>(m) == DispatchKind::SideTableResolver);
}

// =============================================================================
// Extensions(m) / Implementations(m) — back-reference snapshots
// =============================================================================

TEST_CASE("RT::Extensions enumerates extensions registered against the impl metaclass",
          AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    auto exts = RT::Extensions(MetaClassOf<AImplT14>);
    // Snapshot stores const MetaClass* — back-reference accessors uniformly speak in metaclass
    // pointers (symmetric with ProviderClass / EagerExtensions). std::ranges::any_of avoids the
    // need to instantiate a transform_view iterator over the span.
    REQUIRE(std::ranges::any_of(exts,
                                [](const MetaClass* p) { return p == &MetaClassOf<BExtT14>; }));
}

TEST_CASE("RT::Implementations lists every implementation of an interface metaclass", AUTO_TAG) {
    Registry::Install<AImplT14>();
    Registry::Install<BExtT14>();

    // AImplT14 is the only Implementation in this fixture set that advertises IAT14. The walk
    // happens inside Registry::Install<AImplT14>() — for each entry in kImplementsInfos<AImplT14>
    // it appends &MetaClassOf<AImplT14> to the interface's implementedBy snapshot.
    auto impls = RT::Implementations(MetaClassOf<IAT14>);
    REQUIRE(std::ranges::any_of(impls,
                                [](const MetaClass* p) { return p == &MetaClassOf<AImplT14>; }));
}

// =============================================================================
// EagerExtensions — projects extensionClass out of the eager-set snapshot
// =============================================================================

TEST_CASE("RT::EagerExtensions enumerates Anno::Eager extensions", AUTO_TAG) {
    Registry::Install<AImplEagerT14>();
    Registry::Install<BExtEagerT14>();

    auto eager = RT::EagerExtensions(MetaClassOf<AImplEagerT14>);
    bool seen = false;
    for (const MetaClass* mc : eager) {
        if (mc == &MetaClassOf<BExtEagerT14>) {
            seen = true;
            break;
        }
    }
    REQUIRE(seen);
}

