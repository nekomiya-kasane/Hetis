#include <Yuki/Core/ArmRegistry.h>
#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace Yuki;

namespace {
    // ---- Interfaces -------------------------------------------------------------------

    struct [[=Anno::Interface]] ISideExt : RootObject {
        ISideExt(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~ISideExt() override = default;
        virtual int Magic() const = 0;
    };

    struct [[=Anno::Interface]] ICodeExt : RootObject {
        ICodeExt(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~ICodeExt() override = default;
        virtual int Magic() const = 0;
    };

    // ---- Nucleus impls (one per arm — separate provider classes keep the seal check
    //      from confusing the two tests when both run sequentially) ----------------------

    struct [[=Anno::Implementation]] SideImpl : RootObject {
        Y_OBJECT;
        SideImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~SideImpl() override = default;
    };

    struct [[=Anno::Implementation]] CodeImpl : RootObject {
        Y_OBJECT;
        CodeImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~CodeImpl() override = default;
    };

    // ---- SideTableResolver — materialize a fresh facade per call, refcount=1 ----------

    std::atomic<int> gFacadeAlive{0};

    struct SideFacade : ISideExt {
        SideFacade()
          : ISideExt(ClassType::None, /*external=*/false) {
            gFacadeAlive.fetch_add(1, std::memory_order_relaxed);
        }
        ~SideFacade() override {
            gFacadeAlive.fetch_sub(1, std::memory_order_relaxed);
        }
        int Magic() const override { return 7; }
    };

    RootObject* SideResolver(RootObject*) noexcept {
        return new SideFacade();    // +1 ref already from RootObject ctor (external=false).
    }

    // ---- CodeExtensionSingleton — static-lifetime facade, refcount-noop ---------------

    struct CodeSingleton : ICodeExt {
        CodeSingleton()
          : ICodeExt(ClassType::None, /*external=*/true) {}
        ~CodeSingleton() override = default;
        int Magic() const override { return 11; }
    };

    CodeSingleton& TheCodeSingleton() noexcept {
        static CodeSingleton s;
        return s;
    }

    RootObject* CodeSingletonAccess() noexcept {
        return static_cast<RootObject*>(&TheCodeSingleton());
    }
}

TEST_CASE("SideTableResolver materializes a fresh facade per Query; release frees it",
          AUTO_TAG) {
    Registry::Install<SideImpl>();
    RegisterSideTable<SideImpl, ISideExt, &SideResolver>();
    auto impl = MakeOwned<SideImpl>();

    {
        ComPtr<ISideExt> a = Query<ISideExt>(impl.Get());
        REQUIRE(a);
        REQUIRE(a->Magic() == 7);
        REQUIRE(gFacadeAlive.load() == 1);
    }
    // Last ComPtr dropped → facade deleted.
    REQUIRE(gFacadeAlive.load() == 0);

    // Second query allocates a fresh facade — resolver is invoked again.
    {
        ComPtr<ISideExt> b = Query<ISideExt>(impl.Get());
        REQUIRE(b);
        REQUIRE(gFacadeAlive.load() == 1);
    }
    REQUIRE(gFacadeAlive.load() == 0);
}

TEST_CASE("CodeExtensionSingleton returns the same pointer across calls", AUTO_TAG) {
    Registry::Install<CodeImpl>();
    RegisterCodeExt<CodeImpl, ICodeExt, &CodeSingletonAccess>();
    auto impl = MakeOwned<CodeImpl>();

    ComPtr<ICodeExt> a = Query<ICodeExt>(impl.Get());
    ComPtr<ICodeExt> b = Query<ICodeExt>(impl.Get());
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.Get() == b.Get());
    REQUIRE(a->Magic() == 11);
    // Singleton outlives every nucleus; dropping ComPtrs is a refcount no-op (external).
}
