/**
 * @file Query.cpp
 * @brief QueryInterface and closure introspection implementation.
 * @ingroup Core
 */
#include "Sora/Kernel/Core/Query.h"

#include "Sora/Kernel/Core/KernelSection.h"
#include "Sora/Kernel/Core/Registry.h"

#include <cassert>
#include <ranges>
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

        [[nodiscard]] DebugProviderCandidate DebugFindProviderCandidate(BaseUnknown* provider, Iid targetIid) {
            if (!provider || IsNil(targetIid)) {
                return {};
            }

            auto meta = provider->GetMeta();
            if (ClassChainContainsIid(meta, targetIid)) {
                return {.providerClass = meta};
            }

            const ProviderEntry* entry = meta ? meta->FindProvide(targetIid) : nullptr;
            return entry ? DebugProviderCandidate{.providerClass = entry->providerClass} : DebugProviderCandidate{};
        }

        void DebugAddLazyExtensionCandidates(DebugProviderCandidate& selected, BaseUnknown* nucleus, Iid targetIid) {
            if (!nucleus) {
                return;
            }
            for (auto extendeeMeta = nucleus->GetMeta(); extendeeMeta; extendeeMeta = extendeeMeta->GetDirectBase()) {
                for (const auto& [_, extensionMeta] : extendeeMeta->Protensions()) {
                    if (!extensionMeta) {
                        continue;
                    }
                    const ProviderEntry* entry = extensionMeta->FindProvide(targetIid);
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

        void DebugAssertUniqueProviderPath(BaseUnknown* object, BaseUnknown* nucleus, Iid targetIid) {
            DebugProviderCandidate selected{};
            DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(object, targetIid));
            if (nucleus && nucleus != object) {
                DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(nucleus, targetIid));
            }
            for (BaseUnknown* extension : Detail::BaseUnknownInternal::SnapshotExtensionNodes(nucleus)) {
                DebugMergeProviderCandidate(selected, DebugFindProviderCandidate(extension, targetIid));
            }
            DebugAddLazyExtensionCandidates(selected, nucleus, targetIid);
        }
#endif

        [[nodiscard]] BaseUnknown* MaterializeProvider(BaseUnknown* provider, const ProviderEntry& entry,
                                                       Iid targetIid) {
            using Detail::BaseUnknownInternal;

            switch (entry.kind) {
            case DispatchKind::Direct:
            case DispatchKind::InlineFacet:
                return entry.factory ? entry.factory(provider) : provider;

            case DispatchKind::BoundFacet:
                if (BaseUnknown* cached = BaseUnknownInternal::FindBoundFacetNode(provider, targetIid)) {
                    return cached;
                }

                if (!entry.factory) {
                    return nullptr;
                }

                BaseUnknown* facet = entry.factory(provider);
                if (!facet) {
                    return nullptr;
                }

                if (BaseUnknownInternal::AdoptBoundFacetNode(provider, targetIid, facet)) {
                    return facet;
                }

                BaseUnknown* existing = BaseUnknownInternal::FindBoundFacetNode(provider, targetIid);
                Release(facet);
                return existing;
            }

            std::unreachable();
        }

        [[nodiscard]] BaseUnknown* QueryProviderObject(BaseUnknown* provider, Iid targetIid) {
            if (!provider || IsNil(targetIid)) {
                return nullptr;
            }

            auto meta = provider->GetMeta();
            if (ClassChainContainsIid(meta, targetIid)) {
                return provider;
            }

            if (const ProviderEntry* entry = meta ? meta->FindProvide(targetIid) : nullptr) {
                return MaterializeProvider(provider, *entry, targetIid);
            }

            return nullptr;
        }

        [[nodiscard]] BaseUnknown* QueryLazyExtensionProvider(BaseUnknown* nucleus, Iid targetIid) {
            if (!nucleus) {
                return nullptr;
            }
            for (auto extendeeMeta = nucleus->GetMeta(); extendeeMeta; extendeeMeta = extendeeMeta->GetDirectBase()) {
                for (const auto& extensionMeta : extendeeMeta->Protensions() | std::views::values) {
                    if (!extensionMeta) {
                        continue;
                    }

                    const ProviderEntry* entry = extensionMeta->FindProvide(targetIid);
                    if (!entry) {
                        continue;
                    }

                    auto MaterializeLazyExtensionProvider =
                        [](BaseUnknown* nucleus, const std::shared_ptr<const MetaClass>& extensionMeta,
                           const ProviderEntry& entry, Iid targetIid) -> BaseUnknown* {
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

                        return extension ? MaterializeProvider(extension, entry, targetIid) : nullptr;
                    };

                    return MaterializeLazyExtensionProvider(nucleus, extensionMeta, *entry, targetIid);
                }
            }
            return nullptr;
        }

    } // namespace

    BaseUnknown* QueryInterfaceRaw(BaseUnknown* object, Iid targetIid) {
        if (!object || IsNil(targetIid)) {
            return nullptr;
        }

        EnsureKernelSectionsRegistered();

        BaseUnknown* nucleus = object->Nucleus();
#ifndef NDEBUG
        DebugAssertUniqueProviderPath(object, nucleus, targetIid);
#endif

        if (BaseUnknown* facet = QueryProviderObject(object, targetIid)) {
            return facet;
        }

        if (nucleus && nucleus != object) {
            if (BaseUnknown* facet = QueryProviderObject(nucleus, targetIid)) {
                return facet;
            }
        }

        for (BaseUnknown* extension : Detail::BaseUnknownInternal::SnapshotExtensionNodes(nucleus)) {
            if (BaseUnknown* facet = QueryProviderObject(extension, targetIid)) {
                return facet;
            }
        }

        return QueryLazyExtensionProvider(nucleus, targetIid);
    }

} // namespace Sora::Kernel
