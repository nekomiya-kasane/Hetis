#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"

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

    namespace Tie {

        template<Concept::ComponentClass Impl>
        class [[= Sora::Kernel::$::TIE]] Tie_IPosition : public IPosition {
        public:
            S_OBJECT

            using Provider = Traits::InterfaceProviderClassOf<IPosition, Impl>;

            void SetPosition(float x, float y) override { static_cast<Provider*>(BoundTarget())->SetPosition(x, y); }
            void GetPosition(float& x, float& y) const override {
                static_cast<const Provider*>(BoundTarget())->GetPosition(x, y);
            }
        };

        template<Concept::ComponentClass Impl>
        class [[= Sora::Kernel::$::TIE]] Tie_I3DPosition : public I3DPosition {
        public:
            S_OBJECT

            using BaseProvider = Traits::InterfaceProviderClassOf<IPosition, Impl>;
            using Provider = Traits::InterfaceProviderClassOf<I3DPosition, Impl>;

            void SetPosition(float x, float y) override {
                static_cast<BaseProvider*>(BoundTarget())->SetPosition(x, y);
            }
            void GetPosition(float& x, float& y) const override {
                static_cast<const BaseProvider*>(BoundTarget())->GetPosition(x, y);
            }
            void SetPosition(float x, float y, float z) override {
                static_cast<Provider*>(BoundTarget())->SetPosition(x, y, z);
            }
            void GetPosition(float& x, float& y, float& z) const override {
                static_cast<const Provider*>(BoundTarget())->GetPosition(x, y, z);
            }
        };

    } // namespace Tie

} // namespace Sora::Kernel
