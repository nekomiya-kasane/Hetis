#include "Common/Classes.h"

#include "Sora/Core/Polymorphism.h"
#include "Sora/Core/ToStyledString.h"
#include "Sora/Kernel/Core/IID.h"
#include "Sora/Kernel/Core/Query.h"

#include <cassert>
#include <iostream>
#include <print>

using namespace Sora::Kernel;

consteval {

    Sora::Polymorphism::Define<IPosition, Position2DImpl>();
}

int main() {

    Sora::Polymorphism::Vtable<IPosition, Position2DImpl> vtable;
    Sora::Polymorphism::Adapter<IPosition, Position2DImpl> adapter;
    auto point = MakeComPtr<Position2DImpl>();
    Position2DImpl* pointImpl = point.Get();
    pointImpl->SetPosition(1.0f, 2.0f);
    adapter.target = pointImpl;

    float x, y;
    adapter.vtable().GetPosition(adapter.target, x, y);

    std::cout << x << ", " << y << std::endl;

    IPosition* rawPosition = QueryInterface<IPosition>(pointImpl);
    auto owningPosition = QueryInterface<IPosition>(point);
    auto borrowedPosition = QueryInterface<IPosition>(Sora::Ref(pointImpl));

    assert(rawPosition != nullptr);
    assert(owningPosition);
    assert(borrowedPosition);

    rawPosition->GetPosition(x, y);
    std::cout << "QueryInterface(raw): " << x << ", " << y << std::endl;

    owningPosition->GetPosition(x, y);
    std::cout << "QueryInterface(ComPtr): " << x << ", " << y << std::endl;

    borrowedPosition->GetPosition(x, y);
    std::cout << "QueryInterface(RefPtr): " << x << ", " << y << std::endl;

    ITag* rawTag = QueryInterface<ITag>(rawPosition);
    auto owningTag = QueryInterface<ITag>(point);
    auto borrowedTag = QueryInterface<ITag>(Sora::Ref(pointImpl));

    assert(rawTag != nullptr);
    assert(owningTag);
    assert(borrowedTag);

    uint32_t tag{};
    rawTag->SetTag(42);
    rawTag->GetTag(tag);
    std::cout << "Extension QueryInterface(IPosition*): " << tag << std::endl;

    owningTag->SetTag(7);
    borrowedTag->GetTag(tag);
    std::cout << "Extension QueryInterface(RefPtr): " << tag << std::endl;

    auto solid = MakeComPtr<Position3DImpl>();
    Position3DImpl* solidImpl = solid.Get();
    solidImpl->SetPosition(3.0f, 4.0f, 5.0f);

    I3DPosition* raw3DPosition = QueryInterface<I3DPosition>(solidImpl);
    ITag* inheritedTag = QueryInterface<ITag>(raw3DPosition);

    assert(raw3DPosition != nullptr);
    assert(inheritedTag != nullptr);

    inheritedTag->SetTag(99);
    inheritedTag->GetTag(tag);
    std::cout << "Extension QueryInterface(base extendee): " << tag << std::endl;

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
