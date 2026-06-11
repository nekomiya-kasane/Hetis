#include <Mashiro/Platform/ThreadContract.h>

#include <meta>

#include <catch2/catch_test_macros.hpp>

#include <Support/Meta.h>

// clang-format off

using namespace Mashiro::Traits;
using namespace Mashiro::Platform;

struct [[=OnPlatformThread]] OnPlatformManager {};
struct [[=OnDedicatedThread]] OnDedicatedManager {};
struct [[=OnFreeThreaded]] OnFreeManager {};

TEST_CASE("Thread contract attributes are present and correctly interpreted", AUTO_TAG) {
    STATIC_REQUIRE(GetScheduleMode<OnPlatformManager>() == ScheduleDomain::PlatformThread);
    STATIC_REQUIRE(GetScheduleMode<OnDedicatedManager>() == ScheduleDomain::DedicatedThread);
    STATIC_REQUIRE(GetScheduleMode<OnFreeManager>() == ScheduleDomain::FreeThreaded);
    STATIC_REQUIRE(GetScheduleMode<OnFreeManager>() != ScheduleDomain::DedicatedThread);
}

// clang-format on
