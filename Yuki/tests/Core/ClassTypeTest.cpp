#include <Yuki/Core/ClassType.h>
#include <Yuki/Core/Config.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace Yuki;

TEST_CASE("ClassType enum has the six Y2 roles", AUTO_TAG) {
    static_assert(static_cast<int>(ClassType::None) == 0);
    static_assert(static_cast<int>(ClassType::Interface) == 1);
    static_assert(static_cast<int>(ClassType::Implementation) == 2);
    static_assert(static_cast<int>(ClassType::Extension) == 3);
    static_assert(static_cast<int>(ClassType::Imposter) == 4);
    static_assert(static_cast<int>(ClassType::Bridge) == 5);
    static_assert(std::is_same_v<std::underlying_type_t<ClassType>, std::uint8_t>);
}

TEST_CASE("Yuki::kDebug is a constexpr bool", AUTO_TAG) {
    static_assert(std::is_same_v<decltype(kDebug), const bool>);
    constexpr bool _ = kDebug; (void)_;
}
