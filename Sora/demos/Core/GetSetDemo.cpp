#include "Sora/Core/GetSet.h"

#include <iostream>

struct Point {
    float x;
    float y;
};

class PointExt : public Point {
public:
    PointExt(float x, float y, float z, float w, float e) : Point{x, y}, z(z), w(w), e(e) {}

    [[= Sora::$::Exposed]] float z;

    float GetW() const { return w; }

private:
    ALLOW_GET_SET

    [[= Sora::$::Exposed]] float w;
    float e;
};

int main() {
    using namespace Sora::Literals;

    Point p{1.0f, 2.0f};

    // Get the value of x using Meta::Get
    float xValue = Sora::Meta::Get<^^Point::x>(p);
    std::cout << "x: " << xValue << std::endl;

    // Set the value of y using Meta::Set
    Sora::Meta::Set<^^Point::y>(p, 3.0f);
    std::cout << "y: " << p.y << std::endl;

    // Get the value of z using Meta::Get
    PointExt pe{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    float zValue = Sora::Meta::Get<^^PointExt::z>(pe);
    std::cout << "z: " << zValue << std::endl;

    // Set the value of z using Meta::Set
    Sora::Meta::Set<^^PointExt::z>(pe, 6.0f);
    std::cout << "z: " << pe.z << std::endl;

    Sora::Meta::Set<PointExt, "w"_FS>(pe, 7.0f);
    float wValue = Sora::Meta::Get<"w"_FS>(pe);
    std::cout << "w: " << wValue << std::endl;

    float& ww = Sora::GetRef<float>(pe, "w"_FS);
    ww = 8.0f;
    std::cout << "w: " << pe.GetW() << std::endl;

    Sora::Set(pe, "w"_FS, 9.0f);
    std::cout << "w: " << pe.GetW() << std::endl;

    return 0;
}
