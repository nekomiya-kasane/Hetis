#include "Sora/Kernel/Core/IID.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include <print>

using namespace Sora::Kernel::$;

struct [[= Interface]] IPosition {
    virtual float GetX() const = 0;
    virtual float GetY() const = 0;
};

struct [[= Interface]] IName : IPosition {
    virtual std::string GetName() const = 0;
};

struct [[= Interface]] IName2 {
    virtual std::string GetName() const = 0;
};

struct [[= IidOverride{
    Sora::Kernel::Iid{0x12345678, 0x9abc, 0xdef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}}}]] IName2Override {
    virtual std::string GetName() const = 0;
};

int main() {
    Sora::Kernel::Iid iid = Sora::Kernel::Traits::IidOf<IPosition>;
    std::println("Interface IPosition has IID: {}", iid.ToString());
    Sora::Kernel::Iid iidName = Sora::Kernel::Traits::IidOf<IName>;
    std::println("Interface IName has IID: {}", iidName.ToString());
    Sora::Kernel::Iid iidName2 = Sora::Kernel::Traits::IidOf<IName2>;
    std::println("Interface IName2 has IID: {}", iidName2.ToString());
    Sora::Kernel::Iid iidName2Override = Sora::Kernel::Traits::IidOf<IName2Override>;
    std::println("Interface IName2Override has IID: {}", iidName2Override.ToString());
    return 0;
}