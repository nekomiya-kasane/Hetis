/**
 * @file Query.h
 * @brief QueryInterface adapters for raw pointers, owning component handles, and borrowed component handles.
 * @ingroup Core
 */
#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ComPtr.h"
#include "Sora/Core/RefPtr.h"

#include <memory>
#include <type_traits>

namespace Sora::Kernel {

    /** @brief Type-erased QueryInterface kernel returning a borrowed BaseUnknown-backed facet or extension node. */
    [[nodiscard]] BaseUnknown* QueryInterfaceRaw(BaseUnknown* object, Iid targetIid);

    /** @brief Query @p object for @p Target and return a borrowed raw interface or extension pointer. */
    template<Concept::QueryTargetClass Target, Concept::ComClass T>
        requires(!std::is_const_v<T>)
    [[nodiscard]] Target* QueryInterface(T* object) {
        return static_cast<Target*>(QueryInterfaceRaw(object, Traits::IidOf<Target>));
    }

    /** @brief Query @p object for @p Target and return a borrowed raw interface or extension pointer. */
    template<Concept::QueryTargetClass Target, Concept::ComClass T>
        requires(!std::is_const_v<T>)
    [[nodiscard]] Target* QueryInterface(T& object) {
        return QueryInterface<Target>(std::addressof(object));
    }

    /** @brief Query an owning component handle and return an owning interface or extension handle. */
    template<Concept::QueryTargetClass Target, Concept::ComClass T>
    [[nodiscard]] ComPtr<Target> QueryInterface(const ComPtr<T>& object) {
        if (!object) {
            return nullptr;
        }
        Target* target = QueryInterface<Target>(object.Get());
        return target ? ComPtr<Target>{target} : ComPtr<Target>{nullptr};
    }

    /** @brief Query a borrowed component handle and return a borrowed interface or extension handle. */
    template<Concept::QueryTargetClass Target, Concept::ComClass T>
    [[nodiscard]] Sora::RefPtr<Target> QueryInterface(Sora::RefPtr<T> object) {
        if (!object) {
            return nullptr;
        }
        return Sora::RefPtr<Target>{QueryInterface<Target>(object.Get())};
    }

} // namespace Sora::Kernel
