#include <catch2/catch_all.hpp>

#include "nova/core/Result.h"

TEST_CASE("ErrorCodeTest", "[core]") {
    REQUIRE(static_cast<int>(nova::ErrorCode::None) == 0);
}
