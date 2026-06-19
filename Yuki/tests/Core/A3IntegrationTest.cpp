#include <Yuki/Core/ArmRegistry.h>
#include <Yuki/Core/Closure.h>
#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace Yuki;

namespace {
    // ---- Interfaces — one per arm kind --------------------------------------------------

    struct [[=Anno::Interface]] IInline : RootObject {
        IInline(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IInline() override = default;
        virtual int Inline() const = 0;
    };

    struct [[=Anno::Interface]] ISide : RootObject {
        ISide(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~ISide() override = default;
        virtual int Side() const = 0;
    };

    struct [[=Anno::Interface]] ICode : RootObject {
        ICode(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~ICode() override = default;
        virtual int Code() const = 0;
    };

    // ---- Three-class hierarchy: Base, MidImpl : Base, Leaf : MidImpl --------------------

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IInline}]]
           IntegBase : IInline {
        Y_OBJECT;
        IntegBase()
          : IInline(ClassType::Implementation, /*external=*/false) {}
        ~IntegBase() override = default;
        int Inline() const override { return 1; }
    };

    struct [[=Anno::Implementation]] IntegMid : IntegBase {
        Y_OBJECT;
        IntegMid() : IntegBase() {}
        ~IntegMid() override = default;
    };

    struct [[=Anno::Implementation]] IntegLeaf : IntegMid {
        Y_OBJECT;
        IntegLeaf() : IntegMid() {}
        ~IntegLeaf() override = default;
    };

    // ---- Side-table facade --------------------------------------------------------------

    struct SideFacade : ISide {
        SideFacade() : ISide(ClassType::None, /*external=*/false) {}
        ~SideFacade() override = default;
        int Side() const override { return 2; }
    };
    RootObject* SideResolverInteg(RootObject*) noexcept { return new SideFacade(); }

    // ---- Code-extension singleton -------------------------------------------------------

    struct CodeSing : ICode {
        CodeSing() : ICode(ClassType::None, /*external=*/true) {}
        ~CodeSing() override = default;
        int Code() const override { return 3; }
    };
    CodeSing& TheCodeSing() noexcept { static CodeSing s; return s; }
    RootObject* CodeSingAccess() noexcept { return static_cast<RootObject*>(&TheCodeSing()); }
}

TEST_CASE("A3 integration: three arms + three-class hierarchy + Query + WalkClosure + IidsOf",
          AUTO_TAG) {
    Registry::Install<IntegBase>();
    Registry::Install<IntegMid>();
    Registry::Install<IntegLeaf>();

    RegisterSideTable<IntegBase, ISide,  &SideResolverInteg>();
    RegisterCodeExt  <IntegBase, ICode,  &CodeSingAccess>();

    // Query<I>(base) succeeds for the inline arm — IInline is registered into IntegBase via
    // the static Implements annotation at Install<IntegBase>() time. NOTE: A3 does not yet
    // propagate static-base Implements entries down into subclass mergedDispatch during their
    // Install — that is a separate base-chain-flatten task scoped beyond A3. (Dynamic arms,
    // i.e. RegisterSideTable / RegisterCodeExt, DO propagate via D16's extendedBy walk because
    // they register the arm AFTER subclasses are installed and visible via subclassedBy.)
    auto base = MakeOwned<IntegBase>();
    {
        ComPtr<IInline> a = Query<IInline>(base.Get());
        REQUIRE(a);
        REQUIRE(a->Inline() == 1);
    }

    auto leaf = MakeOwned<IntegLeaf>();

    // ISide + ICode were registered after Mid/Leaf were installed, so the D16 flatten over
    // subclassedBy pushed them into both Mid's and Leaf's mergedDispatch.
    {
        ComPtr<ISide> a = Query<ISide>(leaf.Get());
        REQUIRE(a);
        REQUIRE(a->Side() == 2);
    }
    {
        ComPtr<ICode> a = Query<ICode>(leaf.Get());
        REQUIRE(a);
        REQUIRE(a->Code() == 3);
    }

    // IidsOf(leaf) contains side + code (the D16-propagated dynamic arms).
    auto iids = IidsOf(leaf.Get());
    bool sawSide = false, sawCode = false;
    for (const auto& e : iids) {
        if (e.iid == IidOf<ISide>()) sawSide = true;
        if (e.iid == IidOf<ICode>()) sawCode = true;
    }
    REQUIRE(sawSide);
    REQUIRE(sawCode);

    // WalkClosure(leaf) at minimum enumerates the nucleus itself. Facade/extension side
    // tables ship empty in A3 (per Closure.h §3 ship-state note); A4 will surface the
    // materialised facade + extension nodes here without changing this surface.
    std::vector<RootObject*> visited;
    WalkClosure(leaf.Get(), [&](RootObject* n) { visited.push_back(n); });
    REQUIRE(!visited.empty());
    REQUIRE(visited[0] == leaf.Get());
}
