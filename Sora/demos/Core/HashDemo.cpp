#include "Sora/Core/Hash.h"

#include <iostream>
#include <print>

using namespace Sora;

struct MyStruct {
    int a;
    float b;
    char c;
};

int main() {
    Uuid id{0x12345678, 0x9abc, 0xdef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
    std::string str = id.ToString();
    std::cout << str << std::endl;
    std::println("{}", str);

    MyStruct s{42, 3.14f, 'x'};
    size_t hashValue = std::hash<MyStruct>{}(s);
    std::cout << "Hash value: " << hashValue << std::endl;

    return 0;
}