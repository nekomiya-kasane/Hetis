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
    STATIC_REQUIRE(GetScheduleMode<OnPlatformManager>() == ScheduleMode::PlatformThread);
    STATIC_REQUIRE(GetScheduleMode<OnDedicatedManager>() == ScheduleMode::DedicatedThread);
    STATIC_REQUIRE(GetScheduleMode<OnFreeManager>() == ScheduleMode::FreeThreaded);
    STATIC_REQUIRE(GetScheduleMode<OnFreeManager>() != ScheduleMode::DedicatedThread);
}

// clang-format on
