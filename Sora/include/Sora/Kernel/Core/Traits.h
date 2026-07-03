#pragma once

#include <concepts>
#include <type_traits>

namespace Sora::Kernel {

    /** @brief Root object base for all objects that participate in Yuki lifetime and QueryInterface. */
    class BaseUnknown;

    /** @brief Immutable runtime metaclass record. */
    class MetaClass;

    namespace Concept {

        /** @brief Type that is recognized as a Yuki object-model class. */
        template<typename T>
        concept ComClass =
            std::same_as<std::remove_cvref_t<T>, BaseUnknown> || std::derived_from<std::remove_cvref_t<T>, BaseUnknown>;

    } // namespace Concept

    /** @brief Intrusive owning pointer for BaseUnknown-anchored concrete objects and interface facets. */
    template<Concept::ComClass T>
    class ComPtr;

    /** @brief Allocate @p T, bind its static metaclass, and return an adopted owning pointer. */
    template<Concept::ComClass T, class... Args>
    [[nodiscard]] ComPtr<T> MakeOwned(Args&&... args);

    /** @brief Runtime factory function after a module has been realized. */
    using DefaultFactory = BaseUnknown* (*)();

    /** @brief Type-erased resolver that maps a base object to a BaseUnknown-backed facet. */
    using FacetFactory = BaseUnknown* (*)(BaseUnknown * base) noexcept;

} // namespace Sora::Kernel