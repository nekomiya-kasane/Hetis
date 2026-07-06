/**
 * @file Query.h
 * @brief Intrusive component handles, QueryInterface, closure attachment, and object-model introspection.
 * @ingroup Core
 */
#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ComPtr.h"
#include "Sora/Core/RefPtr.h"

#include <memory>
#include <type_traits>

namespace Sora::Kernel {

    /** @brief Type-erased QueryInterface kernel returning a borrowed BaseUnknown-backed facet. */
    [[nodiscard]] BaseUnknown* QueryInterfaceRaw(BaseUnknown* object, Iid interfaceIid);

    /** @brief Query @p object for @p Iface and return a borrowed raw interface pointer. */
    template<Concept::InterfaceClass Iface, Concept::ComClass T>
        requires(!std::is_const_v<T>)
    [[nodiscard]] Iface* QueryInterface(T* object) {
        return static_cast<Iface*>(QueryInterfaceRaw(object, Traits::IidOf<Iface>));
    }

    /** @brief Query @p object for @p Iface and return a borrowed raw interface pointer. */
    template<Concept::InterfaceClass Iface, Concept::ComClass T>
        requires(!std::is_const_v<T>)
    [[nodiscard]] Iface* QueryInterface(T& object) {
        return QueryInterface<Iface>(std::addressof(object));
    }

    /** @brief Query an owning component handle and return an owning interface handle. */
    template<Concept::InterfaceClass Iface, Concept::ComClass T>
    [[nodiscard]] ComPtr<Iface> QueryInterface(const ComPtr<T>& object) {
        if (!object) {
            return nullptr;
        }
        Iface* iface = QueryInterface<Iface>(object.Get());
        return iface ? ComPtr<Iface>{iface} : ComPtr<Iface>{nullptr};
    }

    /** @brief Query a borrowed component handle and return a borrowed interface handle. */
    template<Concept::InterfaceClass Iface, Concept::ComClass T>
    [[nodiscard]] Sora::RefPtr<Iface> QueryInterface(Sora::RefPtr<T> object) {
        if (!object) {
            return nullptr;
        }
        return Sora::RefPtr<Iface>{QueryInterface<Iface>(object.Get())};
    }

} // namespace Sora::Kernel
