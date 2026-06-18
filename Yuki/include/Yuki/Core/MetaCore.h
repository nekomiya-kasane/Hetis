#pragma once
#include <Yuki/Core/Identity.h>
#include <type_traits>
namespace Yuki {
    struct MetaCore { ClassType role; };
    namespace Detail {
        template <class T> consteval MetaCore MakeMetaCoreFor() {
            return { ClassTypeOf<T> };
        }
        // Empty hook with a consteval ctor whose body asserts the D3.1 invariant.
        // Y_OBJECT plants this as a [[no_unique_address]] NSDMI subobject; NSDMIs
        // are parsed in complete-class context, so T is complete when the
        // consteval ctor body runs at constant evaluation.
        template <class T> struct MetaHook {
            consteval MetaHook() {
                static_assert(std::has_virtual_destructor_v<T>,
                              "Y_OBJECT requires a virtual destructor on the enclosing class");
            }
        };
    }
}
