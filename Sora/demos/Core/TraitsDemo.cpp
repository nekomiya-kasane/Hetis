#include "Sora/Core/Traits.h"

#include <iostream>

enum class Color : uint8_t { Red, Green, Blue };

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

    return 0;
}