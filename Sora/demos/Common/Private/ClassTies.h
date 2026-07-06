/**
 * @file PositionTies.h
 * @brief Private demo TIE facets adapting position interfaces to component implementations.
 * @ingroup Core
 */
#pragma once

#include "Common/Classes.h"

#include "Sora/Kernel/Core/Registry.h"

namespace Sora::Kernel::Tie {

    /** @brief Bound facet adapting a component implementation to @ref IPosition. */
    template<Concept::ComponentClass Impl>
    class [[= Sora::Kernel::$::TIE]] Tie$IPosition : public IPosition {
    public:
        S_OBJECT

        /** @brief Forward to the matching two-dimensional component setter. */
        void SetPosition(float x, float y) override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y);
        }

        /** @brief Forward to the matching two-dimensional component getter. */
        void GetPosition(float& x, float& y) const override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y);
        }
    };

    /** @brief Bound facet adapting a component implementation to @ref I3DPosition. */
    template<Concept::ComponentClass Impl>
    class [[= Sora::Kernel::$::TIE]] Tie$I3DPosition : public I3DPosition {
    public:
        S_OBJECT

        /** @brief Forward to the matching two-dimensional component setter. */
        void SetPosition(float x, float y) override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y);
        }

        /** @brief Forward to the matching two-dimensional component getter. */
        void GetPosition(float& x, float& y) const override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y);
        }

        /** @brief Forward to the matching three-dimensional component setter. */
        void SetPosition(float x, float y, float z) override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y, z);
        }

        /** @brief Forward to the matching three-dimensional component getter. */
        void GetPosition(float& x, float& y, float& z) const override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), x, y, z);
        }
    };

    /** @brief Bound facet adapting a component extension to @ref ITag. */
    template<Concept::ComponentClass Impl>
    class [[= Sora::Kernel::$::TIE]] Tie$ITag : public ITag {
    public:
        S_OBJECT

        /** @brief Forward to the matching tag setter. */
        void SetTag(uint32_t tag) override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), tag);
        }

        /** @brief Forward to the matching tag getter. */
        void GetTag(uint32_t& tag) const override {
            constexpr auto current = std::meta::access_context::current().scope();
            Detail::InvokeTieCurrent<current, Impl>(BoundTarget(), tag);
        }
    };

} // namespace Sora::Kernel::Tie

namespace Sora::Kernel::Tie {

    /** @brief Private TIE mapping for @ref IPosition. */
    template<Concept::ComponentClass Impl>
    struct TieClass<IPosition, Impl> {
        using Type = Tie::Tie$IPosition<Impl>; /**< Concrete bound facet type. */
    };

    /** @brief Private TIE mapping for @ref I3DPosition. */
    template<Concept::ComponentClass Impl>
    struct TieClass<I3DPosition, Impl> {
        using Type = Tie::Tie$I3DPosition<Impl>; /**< Concrete bound facet type. */
    };

    /** @brief Private TIE mapping for @ref ITag. */
    template<Concept::ComponentClass Impl>
    struct TieClass<ITag, Impl> {
        using Type = Tie::Tie$ITag<Impl>; /**< Concrete bound facet type. */
    };

} // namespace Sora::Kernel::Tie
