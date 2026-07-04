#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"

class [[= Sora::Kernel::$::Implementation]] Position2DImpl : public Sora::Kernel::BaseUnknown {
public:
    S_OBJECT

    void SetPosition(float x, float y) {
        x_ = x;
        y_ = y;
    }

    void GetPosition(float& x, float& y) const {
        x = x_;
        y = y_;
    }

private:
    float x_{0.0f}, y_{0.0f};
};

class [[= Sora::Kernel::$::Implementation]] Position3DImpl : public Position2DImpl {
public:
    void SetPosition(float x, float y, float z) {
        x_ = x;
        y_ = y;
        z_ = z;
    }

    void GetPosition(float& x, float& y, float& z) const {
        x = x_;
        y = y_;
        z = z_;
    }

private:
    float x_{0.0f}, y_{0.0f}, z_{0.0f};
};

class [[= Sora::Kernel::$::Interface]] IPosition : public Sora::Kernel::BaseUnknown {
public:
    virtual ~IPosition() noexcept = default;

    virtual void SetPosition(float x, float y) = 0;
    virtual void GetPosition(float& x, float& y) const = 0;
};

class [[= Sora::Kernel::$::Interface]] I3DPosition : public IPosition {
public:
    virtual ~I3DPosition() noexcept = default;

    virtual void SetPosition(float x, float y, float z) = 0;
    virtual void GetPosition(float& x, float& y, float& z) const = 0;
};
