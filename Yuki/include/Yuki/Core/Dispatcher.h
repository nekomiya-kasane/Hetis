/**
 * @file Dispatcher.h
 * @brief Provider dispatch kinds and type-erased BaseUnknown facet resolver entries.
 * @ingroup Core
 */
#pragma once

#include <cstdint>

#include "Yuki/Core/IID.h"
#include "Yuki/Core/Types.h"

namespace Yuki {

    class BaseUnknown;
    class MetaClass;

    /** @brief Runtime dispatch shape for a component-interface provider. */
    enum class DispatchKind : uint8_t {
        Direct = 0,                 /**< Interface is the provider object itself. */
        InlineFacet = 1,            /**< Facet is stored inline inside the provider frame. */
        BoundFacet = 2,             /**< Facet is a closure-owned bound object. */
        AttachedExtension = 3,      /**< Facet is provided by an attached extension object. */
        CodeExtensionSingleton = 4, /**< Facet is provided by a singleton code extension. */
        TransientProvider = 5,      /**< Facet is produced by a transient provider. */
    };

    /** @brief Type-erased resolver that maps a provider object to a BaseUnknown-backed facet. */
    using FacetResolver = BaseUnknown* (*)(BaseUnknown* provider) noexcept;

    namespace Anno {

        /** @brief Declares provider dispatch policy and priority. */
        struct Dispatch {
            DispatchKind kind{DispatchKind::InlineFacet}; /**< Provider lowering shape. */
            uint32_t priority{2};                         /**< Lower value wins when multiple providers exist. */
        };

    } // namespace Anno

    /** @brief One runtime provider edge from a component class to an interface. */
    struct ProviderEntry {
        Iid interfaceIid{};                           /**< Interface provided by this entry. */
        DispatchKind kind{DispatchKind::InlineFacet}; /**< Runtime dispatch shape. */
        FacetResolver resolve{};                      /**< Resolver used after the provider object is known. */
        const MetaClass* providerClass{};             /**< Class that contributed the provider, if known. */
        uint32_t priority{2};                         /**< Lower value wins when providers are merged. */
    };

} // namespace Yuki