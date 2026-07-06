/**
 * @file Query.cpp
 * @brief QueryInterface and closure introspection implementation.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/Query.h"

#include "Sora/Kernel/Core/Registry.h"

#include <utility>

namespace Sora::Kernel {

    namespace {

        [[nodiscard]] bool ClassChainContainsIid(std::shared_ptr<const MetaClass> meta, Iid iid) {
            for (; meta; meta = meta->GetDirectBase()) {
                if (meta->GetIid() == iid) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] BaseUnknown* MaterializeProvider(BaseUnknown* provider, const ProviderEntry& entry,
                                                       Iid interfaceIid) {
            using Detail::BaseUnknownInternal;

            switch (entry.kind) {
            case DispatchKind::Direct:
            case DispatchKind::InlineFacet:
            case DispatchKind::AttachedExtension:
                return entry.factory ? entry.factory(provider) : provider;

            case DispatchKind::BoundFacet:
                if (BaseUnknown* cached = BaseUnknownInternal::FindBoundFacetNode(provider, interfaceIid)) {
                    return cached;
                }

                if (!entry.factory) {
                    return nullptr;
                }

                BaseUnknown* facet = entry.factory(provider);
                if (!facet) {
                    return nullptr;
                }

                if (BaseUnknownInternal::AdoptBoundFacetNode(provider, interfaceIid, facet)) {
                    return facet;
                }

                BaseUnknown* existing = BaseUnknownInternal::FindBoundFacetNode(provider, interfaceIid);
                Release(facet);
                return existing;
            }

            std::unreachable();
        }

        [[nodiscard]] BaseUnknown* QueryProviderObject(BaseUnknown* provider, Iid interfaceIid) {
            if (!provider || IsNil(interfaceIid)) {
                return nullptr;
            }

            auto meta = provider->GetMeta();
            if (ClassChainContainsIid(meta, interfaceIid)) {
                return provider;
            }

            if (const ProviderEntry* entry = meta ? meta->FindProvide(interfaceIid) : nullptr) {
                return MaterializeProvider(provider, *entry, interfaceIid);
            }

            return nullptr;
        }

    } // namespace

    const ProviderEntry* MetaClass::FindProvide(Iid iid) const noexcept {
        for (const MetaClass* current = this; current != nullptr; current = current->base.get()) {
            if (auto found = current->provides.find(iid); found != current->provides.end()) {
                return std::addressof(found->second);
            }
        }
        return nullptr;
    }

    BaseUnknown* QueryInterfaceRaw(BaseUnknown* object, Iid interfaceIid) {
        if (!object || IsNil(interfaceIid)) {
            return nullptr;
        }

        if (BaseUnknown* facet = QueryProviderObject(object, interfaceIid)) {
            return facet;
        }

        BaseUnknown* nucleus = object->Nucleus();
        if (nucleus && nucleus != object) {
            if (BaseUnknown* facet = QueryProviderObject(nucleus, interfaceIid)) {
                return facet;
            }
        }

        for (BaseUnknown* extension : Detail::BaseUnknownInternal::SnapshotExtensionNodes(nucleus)) {
            if (BaseUnknown* facet = QueryProviderObject(extension, interfaceIid)) {
                return facet;
            }
        }

        return nullptr;
    }

} // namespace Sora::Kernel
