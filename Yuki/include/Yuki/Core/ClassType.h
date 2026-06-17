#pragma once
#include <cstdint>
namespace Yuki {
    enum class ClassType : std::uint8_t {
        None           = 0,
        Interface      = 1,
        Implementation = 2,
        Extension      = 3,
        Imposter       = 4,
        Bridge         = 5,
    };
}
