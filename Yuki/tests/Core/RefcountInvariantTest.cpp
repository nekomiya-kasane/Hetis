#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/MakeOwned.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace Yuki;

namespace {
    struct Impl : RootObject {
        Impl() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~Impl() override = default;
    };

    // Hand-rolled facade analogue: pins its underlying RootObject for its own lifetime
    // by Acquire-ing in the ctor and Release-ing (then deleting on 0-transition) in the dtor.
    // This is the propagation primitive that A2's real dispatch arms will use.
    struct Facadeish : RootObject {
        RootObject* underlying;
        explicit Facadeish(RootObject* u)
          : RootObject(ClassType::Interface, nullptr, /*external=*/false), underlying(u)
        { Acquire(underlying); }
        ~Facadeish() override { if (Release(underlying)) delete underlying; }
    };
}

TEST_CASE("Facade-like Acquire pins underlying for its lifetime", AUTO_TAG) {
    auto impl = MakeOwned<Impl>();
    Impl* rawImpl = impl.Get();
    REQUIRE(rawImpl->PayloadRelaxed().refcount() == 1);
    {
        auto f = MakeOwned<Facadeish>(rawImpl);
        REQUIRE(rawImpl->PayloadRelaxed().refcount() == 2);   // facade pinned underlying
        REQUIRE(f->PayloadRelaxed().refcount() == 1);
        REQUIRE(f->PayloadRelaxed().refcount() >= rawImpl->PayloadRelaxed().refcount() - 1);
    }
    REQUIRE(rawImpl->PayloadRelaxed().refcount() == 1);       // facade gone, underlying released
}

TEST_CASE("Saturation guard at kSaturationLimit", AUTO_TAG) {
    auto impl = MakeOwned<Impl>();
    auto& w = impl->MetaWord();
    auto cur = w.load();
    cur.word = (cur.word & ~(0xFFFFull << 4))
             | (std::uint64_t(TaggedPayload::kSaturationLimit - 1) << 4);
    w.store(cur);
    REQUIRE(TaggedPayload::TryIncrement(w) == true);
    REQUIRE(TaggedPayload::TryIncrement(w) == false);

    // Restore refcount to 1 so ComPtr's destructor can transition to 0 and delete impl —
    // otherwise TryDecrement decrements to kSaturationLimit-1, returns false, and impl leaks.
    cur = w.load();
    cur.word = (cur.word & ~(0xFFFFull << 4)) | (std::uint64_t(1) << 4);
    w.store(cur);
}
