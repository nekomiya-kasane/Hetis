/**
 * @file Query.cpp
 * @brief QueryInterface and closure introspection implementation.
 * @ingroup Core
 */
#include <Yuki/Core/Query.h>

namespace Yuki {

    namespace {

        /** @brief Resolved provider candidate visible from one closure node. */
        struct ProviderCandidate {
            BaseUnknown* provider{};                  /**< Provider object that contributed the candidate. */
            BaseUnknown* facet{};                     /**< Facet object returned by the provider resolver. */
            const MetaClass* providerClass{};         /**< Metaclass that contributed the provider entry. */
            std::uint32_t priority{};                 /**< Lower value wins across the whole closure. */
        };

        /** @brief Return whether @p meta is @p iid or derives from it through materialized direct-base links. */
        [[nodiscard]] bool MetaIsKindOf(const MetaClass& meta, Iid iid) noexcept {
            for (const MetaClass* current = &meta; current; current = current->DirectBase()) {
                if (current->IidValue() == iid) {
                    return true;
                }
                if (!current->DirectBase() && current->DirectBaseIid() == iid) {
                    return true;
                }
            }
            return false;
        }

        /** @brief Find the best provider for @p iid on @p meta or one of its object-model bases. */
        [[nodiscard]] const ProviderEntry* FindProvideInMetaChain(const MetaClass& meta, Iid iid,
                                                                  const MetaClass*& providerClass) noexcept {
            const ProviderEntry* best = nullptr;
            providerClass = nullptr;
            for (const MetaClass* current = &meta; current; current = current->DirectBase()) {
                const ProviderEntry* entry = current->FindProvide(iid);
                if (entry && (!best || entry->priority < best->priority)) {
                    best = entry;
                    providerClass = entry->providerClass ? entry->providerClass : current;
                }
            }
            return best;
        }

        /** @brief Append direct and inherited provider entries visible on @p provider to @p entries. */
        void AppendProviderEntries(std::vector<ProviderEntry>& entries, const BaseUnknown* provider) {
            for (const MetaClass* current = &provider->GetMeta(); current; current = current->DirectBase()) {
                for (ProviderEntry entry : current->Provides()) {
                    if (!entry.providerClass) {
                        entry.providerClass = current;
                    }
                    entries.push_back(entry);
                }
            }
        }

        /** @brief Return a concrete-class candidate when @p provider itself is the requested class. */
        [[nodiscard]] ProviderCandidate ConcreteCandidate(BaseUnknown* provider, Iid iid) noexcept {
            if (!provider || !MetaIsKindOf(provider->GetMeta(), iid)) {
                return {};
            }
            return ProviderCandidate{provider, provider, &provider->GetMeta(), 0};
        }

        /** @brief Resolve @p iid from a concrete provider object through its metaclass inheritance chain. */
        [[nodiscard]] ProviderCandidate ResolveFromProvider(BaseUnknown* provider, Iid iid) noexcept {
            if (!provider) {
                return {};
            }
            if (iid == IidOf<BaseUnknown>()) {
                return ProviderCandidate{provider, provider->Nucleus(), &BaseUnknownMetaClass(), 0};
            }
            if (ProviderCandidate concrete = ConcreteCandidate(provider, iid); concrete.facet) {
                return concrete;
            }

            const MetaClass* providerClass = nullptr;
            const ProviderEntry* entry = FindProvideInMetaChain(provider->GetMeta(), iid, providerClass);
            BaseUnknown* facet = entry && entry->resolve ? entry->resolve(provider) : nullptr;
            if (!facet) {
                return {};
            }
            return ProviderCandidate{provider, facet, providerClass, entry->priority};
        }

        /** @brief Return the better visible provider candidate, preserving discovery order on equal priority. */
        [[nodiscard]] ProviderCandidate Prefer(ProviderCandidate best, ProviderCandidate candidate) noexcept {
            if (!candidate.facet) {
                return best;
            }
            if (!best.facet || candidate.priority < best.priority) {
                return candidate;
            }
            return best;
        }

    } // namespace

    BaseUnknown* Nucleus(BaseUnknown* object) noexcept {
        return object ? object->Nucleus() : nullptr;
    }

    const BaseUnknown* Nucleus(const BaseUnknown* object) noexcept {
        return object ? object->Nucleus() : nullptr;
    }

    bool InClosure(const BaseUnknown* lhs, const BaseUnknown* rhs) noexcept {
        return lhs && rhs && lhs->Nucleus() == rhs->Nucleus();
    }

    PinnedView<BaseUnknown*> Extensions(const BaseUnknown* object) {
        if (!object) {
            return PinnedView<BaseUnknown*>{};
        }
        return PinnedView<BaseUnknown*>{object->Nucleus()->CopyExtensionNodes()};
    }

    PinnedView<BaseUnknown*> BoundFacets(const BaseUnknown* object) {
        if (!object) {
            return PinnedView<BaseUnknown*>{};
        }
        return PinnedView<BaseUnknown*>{object->Nucleus()->CopyBoundFacetNodes()};
    }

    bool Detail::CanAttachExtension(const BaseUnknown* object, const MetaClass& extensionClass) noexcept {
        if (!object || !IsExtension(extensionClass.GetTypeOfClass()) || extensionClass.Extendees().empty()) {
            return false;
        }
        const MetaClass& objectClass = object->Nucleus()->GetMeta();
        for (Iid extendee : extensionClass.Extendees()) {
            if (MetaIsKindOf(objectClass, extendee)) {
                return true;
            }
        }
        return false;
    }

    BaseUnknown* Detail::QueryInterfaceFacet(BaseUnknown* object, Iid iid, BaseUnknown*& retainedAnchor) noexcept {
        retainedAnchor = nullptr;
        if (!object) {
            return nullptr;
        }

        BaseUnknown* nucleus = object->Nucleus();
        ProviderCandidate best = ResolveFromProvider(nucleus, iid);
        nucleus->VisitExtensionNodes([&](BaseUnknown* extension) noexcept {
            best = Prefer(best, ResolveFromProvider(extension, iid));
            return false;
        });
        nucleus->VisitBoundFacetNodes([&](BaseUnknown* facet) noexcept {
            best = Prefer(best, ResolveFromProvider(facet, iid));
            return false;
        });

        if (!best.facet) {
            return nullptr;
        }
        retainedAnchor = best.provider ? best.provider->Nucleus() : nucleus;
        if (!retainedAnchor) {
            return nullptr;
        }
        Retain(retainedAnchor);
        return best.facet;
    }

    const MetaClass* TypeOf(const BaseUnknown* object) noexcept {
        return object ? &object->GetMeta() : nullptr;
    }

    bool Provides(const BaseUnknown* object, Iid iid) noexcept {
        if (!object) {
            return false;
        }
        if (iid == IidOf<BaseUnknown>()) {
            return true;
        }
        const BaseUnknown* nucleus = object->Nucleus();
        const MetaClass* providerClass = nullptr;
        if (MetaIsKindOf(nucleus->GetMeta(), iid) || FindProvideInMetaChain(nucleus->GetMeta(), iid, providerClass)) {
            return true;
        }

        bool found = false;
        nucleus->VisitExtensionNodes([&](BaseUnknown* extension) noexcept {
            found = MetaIsKindOf(extension->GetMeta(), iid) ||
                    FindProvideInMetaChain(extension->GetMeta(), iid, providerClass) != nullptr;
            return found;
        });
        if (found) {
            return true;
        }
        nucleus->VisitBoundFacetNodes([&](BaseUnknown* facet) noexcept {
            found = MetaIsKindOf(facet->GetMeta(), iid) ||
                    FindProvideInMetaChain(facet->GetMeta(), iid, providerClass) != nullptr;
            return found;
        });
        return found;
    }

    PinnedView<ProviderEntry> IidsOf(const BaseUnknown* object) {
        if (!object) {
            return PinnedView<ProviderEntry>{};
        }
        const BaseUnknown* nucleus = object->Nucleus();
        std::vector<ProviderEntry> entries;
        AppendProviderEntries(entries, nucleus);
        nucleus->VisitExtensionNodes([&](BaseUnknown* extension) {
            AppendProviderEntries(entries, extension);
            return false;
        });
        nucleus->VisitBoundFacetNodes([&](BaseUnknown* facet) {
            AppendProviderEntries(entries, facet);
            return false;
        });
        return PinnedView<ProviderEntry>{std::move(entries)};
    }

    const MetaClass* ProviderClass(const BaseUnknown* object, Iid iid) noexcept {
        if (!object) {
            return nullptr;
        }
        const BaseUnknown* nucleus = object->Nucleus();
        const MetaClass* providerClass = nullptr;
        ProviderCandidate best = ResolveFromProvider(const_cast<BaseUnknown*>(nucleus), iid);
        nucleus->VisitExtensionNodes([&](BaseUnknown* extension) noexcept {
            best = Prefer(best, ResolveFromProvider(extension, iid));
            return false;
        });
        nucleus->VisitBoundFacetNodes([&](BaseUnknown* facet) noexcept {
            best = Prefer(best, ResolveFromProvider(facet, iid));
            return false;
        });
        if (best.providerClass) {
            return best.providerClass;
        }
        static_cast<void>(FindProvideInMetaChain(nucleus->GetMeta(), iid, providerClass));
        return providerClass;
    }

} // namespace Yuki
