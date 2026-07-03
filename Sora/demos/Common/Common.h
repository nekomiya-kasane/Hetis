#pragma once

#include <Sora/Core/FixedString.h>

#include <string>

enum class Color : uint8_t { Red, Green, Blue };

namespace $ {

    struct AnnoA {
        int value;
    };

    struct AnnoB {
        ::Sora::FixedString<32> name;
    };

    constexpr AnnoA a1{42};
    constexpr AnnoA a2{100};

    constexpr AnnoB b1{"b1::Hello"};
    constexpr AnnoB b2{"b2::World"};

} // namespace $

// Example usage of the Traits library
struct MyStruct {
    int aa;
    double bb;
    std::string cc;
    [[= Sora::$::Ignore{}]] double extra;
    std::string dd;
};

struct A {
    int x;
};

struct B : A {
    double y;
};

struct C : B {
    std::string z;
};

struct D {
    int w;
};

struct E : C, D {
    struct F {
        using G = int;
    } f;

    float v;
};