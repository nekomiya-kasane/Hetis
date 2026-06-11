#pragma once

#include <string>

namespace Mashiro::Anno {

    struct Required {};
    struct Optional {};
    
    struct Repr {
        std::string label;
        std::string description;
        std::string key;

        constexpr bool operator==(const Repr&) const = default;
    };

    struct RangeInt {
        int min = 0;
        int max = 0;
        constexpr bool operator==(const RangeInt&) const = default;
    };

    struct RangeFloat {
        float min = 0.0f;
        float max = 0.0f;
        constexpr bool operator==(const RangeFloat&) const = default;
    };

}
