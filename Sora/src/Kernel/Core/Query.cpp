/**
 * @file Query.cpp
 * @brief QueryInterface and closure introspection implementation.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/Query.h"

#include "Sora/Kernel/Core/ProviderSection.h"
#include "Sora/Kernel/Core/Registry.h"

#include <cassert>
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

#ifndef NDEBUG
        struct DebugProviderCandidate {
            std::shared_ptr<const MetaClass> providerClass{};
        };

        [[nodiscard]] bool IsSameOrDerivedClass(std::shared_ptr<const MetaClass> derived,
                                                std::shared_ptr<const MetaClass> base) {
            return derived && base && ClassChainContainsIid(std::move(derived), base->GetIid());
        }

        void DebugMergeProviderCandidate(DebugProviderCandidate& selected, DebugProviderCandidate candidate) {
            if (!candidate.providerClass) {
                return;
            }
            if (!selected.providerClass) {
                selected = candidate;
                return;
            }

            if (selected.providerClass->GetIid() == candidate.providerClass->GetIid()) {
                return;
            }

            if (IsSameOrDerivedClass(candidate.providerClass, selected.providerClass)) {
                selected = candidate;
                return;
            }
            if (IsSameOrDerivedClass(selected.providerClass, candidate.providerClass)) {
                return;
            }

            assert(false && "ambiguous QueryInterface providers: providers must be on one inheritance chain");
        }

        [[nodiscard]] DebugProviderCandidate DebugFindProviderCandidate(BaseUnknown* provider, Iid interfaceIid) {
            if (!provider || IsNil(interfaceIid)) {
                return {};
            }

            auto meta = provider->GetMeta();
            if (ClassChainContainsIid(meta, interfaceIid)) {
                return {.providerClass = meta};
            }

            const ProviderEntry* entry = meta ? meta->FindProvide(interfaceIid) : nullptr;
            return entry ? DebugProviderCandidate{.providerClass = entry->providerClass} : DebugProviderCandidate{};
        }

        void DebugAddLazyExtensionCandidates(DebugProviderCandidate& selected, BaseUnknown* nucleus, Iid interfaceIid) {
            if (!nucleus) {
                return;
            }
            for (auto extendeeMeta = nucleus->GetMeta(); extendeeMeta; extendeeMeta = extendeeMeta->GetDirectBase()) {
                for (const auto& [_, extensionMeta] : extendeeMeta->Protensions()) {
                    if (!extensionMeta) {
                        continue;
                    }
                    const ProviderEntry* entry = extensionMeta->FindProvide(interfaceIid);
                    if (!entry) {
                        continue;
                    }
                    assert(entry->extensionFactory &&
                           "lazy extension providers must have ProviderEntry::extensionFactory");
                    assert(entry->providerClass && IsExtension(entry->providerClass->GetTypeOfClass()));
                    DebugMergeProviderCandidate(selected, {.providerClass = entry->providerClass});
                }
            }
        }

        void DebugAssertUniqueProviderPath(BaseUnknown* object, BaseUnknown* nucleus, Iid interfaceIid) {
            DebugProviderCandidate selected{};
            DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(object, interfaceIid));
            if (nucleus && nucleus != object) {
                DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(nucleus, interfaceIid));
            }
            for (BaseUnknown* extension : Detail::BaseUnknownInternal::SnapshotExtensionNodes(nucleus)) {
                DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(extension, interfaceIid));
            }
            DebugAddLazyExtensionCandidates(selected, nucleus, interfaceIid);
        }
#endif

        [[nodiscard]] BaseUnknown* MaterializeProvider(BaseUnknown* provider, const ProviderEntry& entry,
                                                       Iid interfaceIid) {
            using Detail::BaseUnknownInternal;

            switch (entry.kind) {
            case DispatchKind::Direct:
            case DispatchKind::InlineFacet:
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

        [[nodiscard]] BaseUnknown* QueryLazyExtensionProvider(BaseUnknown* nucleus, Iid interfaceIid) {
            if (!nucleus) {
                return nullptr;
            }
            for (auto extendeeMeta = nucleus->GetMeta(); extendeeMeta; extendeeMeta = extendeeMeta->GetDirectBase()) {
                for (const auto& extensionMeta : extendeeMeta->Protensions() | std::views::values) {
                    if (!extensionMeta) {
                        continue;
                    }

                    const ProviderEntry* entry = extensionMeta->FindProvide(interfaceIid);
                    if (!entry) {
                        continue;
                    }

                    auto MaterializeLazyExtensionProvider =
                        [](BaseUnknown* nucleus, const std::shared_ptr<const MetaClass>& extensionMeta,
                           const ProviderEntry& entry, Iid interfaceIid) -> BaseUnknown* {
                        using BUI = Detail::BaseUnknownInternal;

                        if (!nucleus || !extensionMeta || !entry.extensionFactory) {
                            return nullptr;
                        }

                        const Iid extensionIid = extensionMeta->GetIid();
                        BaseUnknown* extension = BUI::FindExtensionNode(nucleus, extensionIid);
                        if (!extension) {
                            extension = entry.extensionFactory();
                            if (!extension) {
                                return nullptr;
                            }

                            if (!BUI::AdoptExtensionNode(nucleus, extension)) {
                                Release(extension);
                                extension = BUI::FindExtensionNode(nucleus, extensionIid);
                            }
                        }

                        return extension ? MaterializeProvider(extension, entry, interfaceIid) : nullptr;
                    };

                    return MaterializeLazyExtensionProvider(nucleus, extensionMeta, *entry, interfaceIid);
                }
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

        EnsureProviderSectionsRegistered();

        BaseUnknown* nucleus = object->Nucleus();
#ifndef NDEBUG
        DebugAssertUniqueProviderPath(object, nucleus, interfaceIid);
#endif

        if (BaseUnknown* facet = QueryProviderObject(object, interfaceIid)) {
            return facet;
        }

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

        return QueryLazyExtensionProvider(nucleus, interfaceIid);
    }

} // namespace Sora::Kernel
