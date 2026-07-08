#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include "Sora/Kernel/Core/VirtualObject.h"

#include <cstdint>

namespace Sora::Kernel {

    class [[= Sora::Kernel::$::Interface]] IPosition : public BaseUnknown {
    public:
        S_OBJECT

        virtual ~IPosition() noexcept = default;

        virtual void SetPosition(float x, float y) = 0;
        virtual void GetPosition(float& x, float& y) const = 0;
    };

    class [[= Sora::Kernel::$::Interface]] I3DPosition : public IPosition {
    public:
        S_OBJECT

        virtual ~I3DPosition() noexcept = default;

        virtual void SetPosition(float x, float y, float z) = 0;
        virtual void GetPosition(float& x, float& y, float& z) const = 0;
    };

    class [[= Sora::Kernel::$::Interface]] ITag : public BaseUnknown {
    public:
        S_OBJECT

        virtual ~ITag() noexcept = default;

        virtual void SetTag(uint32_t tag) = 0;
        virtual void GetTag(uint32_t& tag) const = 0;
    };

    class [[= Sora::Kernel::$::Implementation, = Sora::Kernel::$::Implements<IPosition>{}]] Position2DImpl
        : public BaseUnknown {
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

    class [[= Sora::Kernel::$::Implementation, = Sora::Kernel::$::Implements<I3DPosition>{}]] Position3DImpl
        : public Position2DImpl {
    public:
        S_OBJECT

        void SetPosition(float x, float y, float z) {
            Position2DImpl::SetPosition(x, y);
            z_ = z;
        }

        void GetPosition(float& x, float& y, float& z) const {
            Position2DImpl::GetPosition(x, y);
            z = z_;
        }

    private:
        float z_{0.0f};
    };

    class [[= Sora::Kernel::$::Implementation]] PointWithoutInterfaceImpl : public BaseUnknown {
    public:
        S_OBJECT
    };

    class [[= Sora::Kernel::$::Implementation]] FutureExtensiblePointImpl : public BaseUnknown {
    public:
        S_OBJECT
    };

    using SceneNode = Virtual<"SceneNode">;

    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<SceneNode>{}]]
    SceneNodeExtension : public BaseUnknown {
    public:
        S_OBJECT
    };

    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<FutureExtensiblePointImpl>{}]]
    FuturePointExtension : public BaseUnknown {
    public:
        S_OBJECT
    };

    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<Position2DImpl>{},
            = Sora::Kernel::$::Implements<ITag>{}]] Position2DExtension : public BaseUnknown {
    public:
        S_OBJECT

        void SetTag(uint32_t tag) { tag_ = tag; }

        void GetTag(uint32_t& tag) const { tag = tag_; }

    private:
        uint32_t tag_{};
    };

    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<PointWithoutInterfaceImpl>{},
            = Sora::Kernel::$::Implements<ITag>{}]] PointTagExtension : public BaseUnknown {
    public:
        S_OBJECT

        void SetTag(uint32_t tag) { tag_ = tag; }

        void GetTag(uint32_t& tag) const { tag = tag_; }

    private:
        uint32_t tag_{};
    };

} // namespace Sora::Kernel
