#include "Common/Classes.h"

#include "Sora/Core/Polymorphism.h"
#include "Sora/Core/ToStyledString.h"
#include "Sora/Kernel/Core/IID.h"

#include <iostream>
#include <print>

using namespace Sora::Kernel;

consteval {

    Sora::Polymorphism::Define<IPosition, Position2DImpl>();
}

int main() {

    Sora::Polymorphism::Vtable<IPosition, Position2DImpl> vtable;
    Sora::Polymorphism::Adapter<IPosition, Position2DImpl> adapter;
    Position2DImpl* pointImpl = new Position2DImpl();
    pointImpl->SetPosition(1.0f, 2.0f);
    adapter.target = pointImpl;

    float x, y;
    adapter.vtable().GetPosition(adapter.target, x, y);

    std::cout << x << ", " << y << std::endl;

    auto meta = pointImpl->GetMeta();
    std::println("Class Name: {}", meta->GetClassName());
    std::println("Class Role: {}", meta->GetTypeOfClass());
    std::println("IID: {}", Traits::IidOf<Position2DImpl>);
    std::cout << "IID: " << Traits::IidOf<Position2DImpl> << std::endl;
    std::cout << "IID: " << meta->GetIid() << std::endl;

    std::cout << "Base Name: " << meta->GetDirectBase()->GetClassName() << std::endl;
    std::cout << "Base Role: " << meta->GetDirectBase()->GetTypeOfClass() << std::endl;
    std::cout << "Base IID: " << meta->GetDirectBase()->GetIid() << std::endl;

    return 0;
}