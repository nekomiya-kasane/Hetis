#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include "Sora/Kernel/Core/IID.h"

#include <print>
#include <string>

class [[= Sora::Kernel::$::Interface]] IPosition : public Sora::Kernel::BaseUnknown {
public:
    S_OBJECT

    virtual ~IPosition() noexcept = default;

    virtual float GetX() const = 0;
    virtual float GetY() const = 0;
};

class [[= Sora::Kernel::$::Interface]] IName : public IPosition {
public:
    S_OBJECT

    virtual ~IName() noexcept = default;

    virtual std::string GetName() const = 0;
};

class [[= Sora::Kernel::$::Interface]] IName2 : public Sora::Kernel::BaseUnknown {
public:
    S_OBJECT

    virtual ~IName2() noexcept = default;

    virtual std::string GetName() const = 0;
};

class [[= Sora::Kernel::$::Interface,
        = Sora::Kernel::$::IidOverride{
            Sora::Kernel::Iid{0x12345678, 0x9abc, 0xdef0, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}}}]]
    IName2Override : public Sora::Kernel::BaseUnknown {
public:
    S_OBJECT

    virtual ~IName2Override() noexcept = default;

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