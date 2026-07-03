#include <string>
#include <iostream>

#include <Sora/Core/ToString.h>
#include <Sora/Core/ToStyledString.h>

#include "Common/Common.h"

int main() {
    MyStruct s{42, 3.14, "Hello", 1.0, "World"};
    std::cout << "MyStruct: " << Sora::ToString(s) << std::endl;
    std::cout << "MyStruct: " << Sora::ToStyledString(s) << std::endl;
    return 0;
}