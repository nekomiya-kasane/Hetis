#include "Sora/Core/Traits.h"
#include "Sora/Core/FixedString.h"

#include <iostream>
#include <variant>

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

int main() {
    // Example usage of the Traits library
    struct MyStruct {
        int aa;
        double bb;
        std::string cc;
    };

    // Get the number of members in MyStruct
    constexpr size_t memberCount = Sora::Traits::MembersCount<MyStruct>;
    std::cout << "MyStruct has " << memberCount << " members." << std::endl;

    // Get the names of the members
    std::cout << "Member " << 0 << ": " << Sora::Traits::MemberIdentifier<MyStruct, 0> << std::endl;
    std::cout << "Member " << 1 << ": " << Sora::Traits::MemberIdentifier<MyStruct, 1> << std::endl;
    std::cout << "Member " << 2 << ": " << Sora::Traits::MemberIdentifier<MyStruct, 2> << std::endl;

    constexpr auto colorEnumerators = Sora::Traits::kEnumNames<Color>;

    for (auto& enumerator : colorEnumerators) {
        std::cout << "Color enumerator: " << enumerator << std::endl;
    }

    std::variant<int, double, MyStruct> v1 = 1;
    std::variant<int, double, MyStruct> v2 = 1.2;
    std::variant<int, double, MyStruct> v3 = MyStruct{42, 3.14, "Hello"};

    auto overload = Sora::Overload{
        [](int i) { std::cout << "int: " << i << std::endl; },
        [](double d) { std::cout << "double: " << d << std::endl; },
        [](const MyStruct& s) {
            std::cout << "MyStruct: aa=" << s.aa << ", bb=" << s.bb << ", cc=" << s.cc << std::endl;
        },
    };

    overload(std::get<int>(v1));
    overload(std::get<double>(v2));
    overload(std::get<MyStruct>(v3));

    std::visit(overload, v1);
    std::visit(overload, v2);
    std::visit(overload, v3);

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

    std::cout << "Direct bases of E: " << Sora::Traits::DirectBasesCount<E> << std::endl;
    std::cout << "Recursive bases of E: " << Sora::Traits::RecursiveBasesCount<E> << std::endl;

    std::cout << "Direct base type of E at index 0: "
              << Sora::Traits::TypeName<typename Sora::Traits::DirectBaseType<E, 0>> << std::endl;
    std::cout << "Direct base type of E at index 1: "
              << Sora::Traits::TypeName<typename Sora::Traits::DirectBaseType<E, 1>> << std::endl;

    std::cout << "Inheritance chain of C: " << Sora::Traits::InheritanceChainIdentifier<C> << std::endl;
    std::cout << "Inheritance chain of C: " << Sora::Traits::InheritanceChainDisplayString<C> << std::endl;

    std::cout << "Scope depth of G: " << Sora::Traits::ScopeDepth<E::F> << std::endl;
    std::cout << "Scope chain identifier of G: " << Sora::Traits::ScopeChainIdentifier<E::F> << std::endl;
    std::cout << "Scope chain display string of G: " << Sora::Traits::ScopeChainDisplayString<E::F> << std::endl;

    std::cout << "Meta scope depth of G: " << Sora::Meta::ScopeDepthOf(^^E::F::G) << std::endl;
    std::cout << "Meta scope chain identifier of G: " << Sora::Meta::ScopeChainIdentifierOf(^^E::F::G) << std::endl;
    std::cout << "Meta scope chain display string of G: " << Sora::Meta::ScopeChainDisplayStringOf(^^E::F::G)
              << std::endl;

    [[= 12]] int x = 10;

    struct[[= $::AnnoA{42}]][[= $::AnnoB{"Hello"}]] H {

    } h;

    struct [[= $::b1]] I {

    } i;

    constexpr auto anno = Sora::$::GetFirst<$::AnnoA>(^^H);
    constexpr auto anno2 = Sora::$::GetFirst<$::AnnoB>(^^H);
    std::cout << "H has AnnoA: " << anno.value().value << std::endl;
    std::cout << "H has AnnoB: " << anno2.value().name.c_str() << std::endl;

    constexpr auto anno3 = Sora::$::GetFirst<$::AnnoB>(^^I);
    std::cout << "I has AnnoB: " << anno3.value().name.c_str() << std::endl;

    return 0;
}