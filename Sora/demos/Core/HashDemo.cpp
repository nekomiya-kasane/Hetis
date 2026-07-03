#include "Sora/Core/Hash.h"

#include <iostream>
#include <print>

using namespace Sora;

struct MyStruct {
    int a;
    float b;
    char c;
    [[=Sora::$::Ignore{}]] int ignoredField;
    struct MySubStruct {
        int x;
        int y;
    } subStruct;
};

int main() {
    Uuid id{0x12345678, 0x9abc, 0xdef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
    std::string str = id.ToString();
    std::cout << str << std::endl;
    std::println("{}", str);

    MyStruct s1{42, 3.14f, 'x', 1, {10, 21}};
    MyStruct s2{43, 3.14f, 'x', 2, {11, 22}};
    MyStruct s3{43, 3.14f, 'y', 3, {12, 20}};
    MyStruct s4{43, 3.14f, 'y', 4, {12, 20}};
    size_t hashValue1 = std::hash<MyStruct>{}(s1);
    size_t hashValue2 = std::hash<MyStruct>{}(s2);
    size_t hashValue3 = std::hash<MyStruct>{}(s3);
    size_t hashValue4 = std::hash<MyStruct>{}(s4);
    std::cout << "Hash value 1: " << std::hex << hashValue1 << std::endl;
    std::cout << "Hash value 2: " << std::hex << hashValue2 << std::endl;
    std::cout << "Hash value 3: " << std::hex << hashValue3 << std::endl;
    std::cout << "Hash value 4: " << std::hex << hashValue4 << std::endl;



    return 0;
}