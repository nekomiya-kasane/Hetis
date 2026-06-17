#pragma once
#include <Yuki/Core/Identity.h>
#include <type_traits>
namespace Yuki {
    struct MetaCore { ClassType role; };
    namespace Detail {
        template <class T> consteval MetaCore MakeMetaCoreFor() {
            return { ClassTypeOf<T> };
        }
        // MetaHook<T> is the deferred-completion hook the Y_OBJECT macro friends
        // into the enclosing class. By the time MetaHook<T> is instantiated, T
        // is complete, so the virtual-dtor invariant (D3) is checked here rather
        // than at class scope where T is still incomplete.
        template <class T> struct MetaHook {
            static_assert(std::has_virtual_destructor_v<T>,
                          "Y_OBJECT requires a virtual destructor on the enclosing class");
        };
    }
}
