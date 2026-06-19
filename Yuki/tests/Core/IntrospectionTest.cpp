#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/MakeOwned.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace Yuki;

namespace {
    // Single-base chain matches QueryTest's pattern (avoids the RootObject diamond when an
    // impl wants multiple Implements entries: each interface introduces a RootObject base).
    // For Introspection we only need ONE real base; additional iids in mergedDispatch come
    // from listing more iids in Anno::Implements. Their dispatch entries land via
    // InlineFacade and a downstream cast would fail — but Introspection is iid-level
    // metadata, not cast, so the extra iids exercise the sort path cleanly.
    struct [[=Anno::Interface]] IIntroA : RootObject {
        IIntroA(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IIntroA() override = default;
    };
    struct [[=Anno::Interface]] IIntroB : RootObject {
        IIntroB(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IIntroB() override = default;
    };
    struct [[=Anno::Interface]] IIntroC : RootObject {
        IIntroC(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IIntroC() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IIntroA, ^^IIntroB}]]
           IntroImpl : IIntroA {
        Y_OBJECT;
        IntroImpl() : IIntroA(ClassType::Implementation, /*external=*/false) {}
        ~IntroImpl() override = default;
    };
}

TEST_CASE("IidsOf returns iid-sorted entries from mergedDispatch", AUTO_TAG) {
    Registry::Install<IntroImpl>();
    auto impl = MakeOwned<IntroImpl>();
    auto entries = IidsOf(impl.Get());
    REQUIRE(entries.size() >= 2);
    REQUIRE(std::is_sorted(entries.begin(), entries.end(),
                           [](const DispatchEntry& a, const DispatchEntry& b) {
                               return a.iid < b.iid;
                           }));
}

TEST_CASE("Provides hits installed iids and misses uninstalled ones", AUTO_TAG) {
    Registry::Install<IntroImpl>();
    auto impl = MakeOwned<IntroImpl>();
    REQUIRE(Provides(impl.Get(), IidOf<IIntroA>()));
    REQUIRE(Provides(impl.Get(), IidOf<IIntroB>()));
    REQUIRE_FALSE(Provides(impl.Get(), IidOf<IIntroC>()));
}

TEST_CASE("ProviderClass and RoleOf agree on the provider for an installed iid", AUTO_TAG) {
    Registry::Install<IntroImpl>();
    auto impl = MakeOwned<IntroImpl>();
    const MetaCore* pc = ProviderClass(impl.Get(), IidOf<IIntroA>());
    REQUIRE(pc == &IntroImpl::kMetaCore);
    REQUIRE(RoleOf(impl.Get(), IidOf<IIntroA>()) == pc->role);
}

TEST_CASE("TypeOf matches node->MetaDyn().core", AUTO_TAG) {
    Registry::Install<IntroImpl>();
    auto impl = MakeOwned<IntroImpl>();
    REQUIRE(TypeOf(impl.Get()) == impl->MetaDyn().core);
    REQUIRE(TypeOf(impl.Get()) == &IntroImpl::kMetaCore);
}

TEST_CASE("Introspection helpers tolerate null and sentinel inputs", AUTO_TAG) {
    REQUIRE(IidsOf(nullptr).empty());
    REQUIRE_FALSE(Provides(nullptr, IidOf<IIntroA>()));
    REQUIRE(ProviderClass(nullptr, IidOf<IIntroA>()) == nullptr);
    REQUIRE(RoleOf(nullptr, IidOf<IIntroA>()) == ClassType::None);
    REQUIRE(TypeOf(nullptr) == nullptr);
}
