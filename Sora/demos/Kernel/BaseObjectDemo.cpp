#include "Common/Classes.h"

#include "Sora/Core/Polymorphism.h"

#include <iostream>

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

    return 0;
}