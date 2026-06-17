#pragma once
namespace Yuki {
    inline constexpr bool kDebug =
#ifdef NDEBUG
        false
#else
        true
#endif
        ;
}
