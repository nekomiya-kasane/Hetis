#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace Yuki;

namespace {
    // Parked-eager probe: a RootObject derivative that bumps a counter on destruction so the
    // test can confirm the nucleus dtor walker freed it.
    std::atomic<int> gExtAlive{0};

    struct ProbeExt : RootObject {
        ProbeExt()
          : RootObject(ClassType::Extension, nullptr, /*external=*/false) {
            gExtAlive.fetch_add(1, std::memory_order_relaxed);
        }
        ~ProbeExt() override {
            gExtAlive.fetch_sub(1, std::memory_order_relaxed);
        }
    };

    struct [[=Anno::Implementation]] DtorNucleus : RootObject {
        Y_OBJECT;
        DtorNucleus()
          : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~DtorNucleus() override = default;
    };

    // Wire an EagerSetSnapshot containing one parked extension onto @p n's MetaLinks.
    void InstallSingleParked(DtorNucleus* n, RootObject* ext) noexcept {
        auto* snap = Detail::AllocEagerSetSnapshot(1);
        const_cast<RootObject**>(snap->parked)[0] = ext;
        n->MetaDyn().links->eagerSet.store(snap, std::memory_order_release);
    }
}

TEST_CASE("DeleteParkedEagers frees a parked extension when the nucleus dies", AUTO_TAG) {
    Registry::Install<DtorNucleus>();

    REQUIRE(gExtAlive.load() == 0);

    // Construct a parked extension and wire it onto a nucleus.
    auto* ext = new ProbeExt();
    ParkEager(ext);                       // refcount: 1 → 0 (parked).
    REQUIRE(gExtAlive.load() == 1);

    {
        auto nucleus = MakeOwned<DtorNucleus>();
        InstallSingleParked(nucleus.Get(), ext);
        // Drop the nucleus ComPtr → refcount 1 → 0 → ~RootObject → DeleteParkedEagers →
        // delete ext. ASan validates the storage was actually freed.
    }
    REQUIRE(gExtAlive.load() == 0);
}

TEST_CASE("DeleteParkedEagers tolerates an unwired nucleus (no eagerSet snapshot)",
          AUTO_TAG) {
    Registry::Install<DtorNucleus>();
    // No eagerSet wired — the walker must be a no-op and not crash on a null snapshot.
    {
        auto nucleus = MakeOwned<DtorNucleus>();
    }
    SUCCEED();
}
