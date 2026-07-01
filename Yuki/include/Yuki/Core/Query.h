/**
 * @file Query.h
 * @brief QueryInterface, closure attachment, bound facet lists, and introspection views.
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Object.h>
#include <Yuki/Core/ComPtr.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Yuki/Core/Dispatcher.h"

namespace Yuki {

    /** @brief Lifetime-pinned immutable span backed by shared vector storage. */
    template<class T>
    class PinnedView {
    public:
        /** @brief Construct an empty pinned view. */
        PinnedView() : owner_(std::make_shared<std::vector<T>>()) {}

        /** @brief Construct a pinned view by taking ownership of @p values. */
        explicit PinnedView(std::vector<T> values) : owner_(std::make_shared<std::vector<T>>(std::move(values))) {}

        /** @brief Return the view as a span. */
        [[nodiscard]] std::span<const T> Span() const noexcept { return *owner_; }

        /** @brief Return an iterator to the first element. */
        [[nodiscard]] const T* begin() const noexcept { return owner_->data(); }

        /** @brief Return an iterator one past the last element. */
        [[nodiscard]] const T* end() const noexcept { return owner_->data() + owner_->size(); }

        /** @brief Return whether the view is empty. */
        [[nodiscard]] bool Empty() const noexcept { return owner_->empty(); }

        /** @brief Return the number of elements in the view. */
        [[nodiscard]] std::size_t Size() const noexcept { return owner_->size(); }

        /** @brief Return the element at @p index. */
        [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return (*owner_)[index]; }

    private:
        std::shared_ptr<const std::vector<T>> owner_;
    };

    /** @brief Return @p object's closure nucleus, or null for a null input. */
    [[nodiscard]] BaseUnknown* Nucleus(BaseUnknown* object) noexcept;

    /** @brief Return @p object's closure nucleus, or null for a null input. */
    [[nodiscard]] const BaseUnknown* Nucleus(const BaseUnknown* object) noexcept;

    /** @brief Return whether @p lhs and @p rhs belong to the same non-null closure nucleus. */
    [[nodiscard]] bool InClosure(const BaseUnknown* lhs, const BaseUnknown* rhs) noexcept;

    /** @brief Return whether two owning handles are anchored in the same non-null closure nucleus. */
    template<class Lhs, class Rhs>
    [[nodiscard]] bool InClosure(const ComPtr<Lhs>& lhs, const ComPtr<Rhs>& rhs) noexcept {
        return lhs.Anchor() && rhs.Anchor() && lhs.Anchor()->Nucleus() == rhs.Anchor()->Nucleus();
    }

    /** @brief Return a lifetime-safe snapshot of extension nodes attached to @p object. */
    [[nodiscard]] PinnedView<BaseUnknown*> Extensions(const BaseUnknown* object);

    /** @brief Return a lifetime-safe snapshot of bound facet nodes attached to @p object. */
    [[nodiscard]] PinnedView<BaseUnknown*> BoundFacets(const BaseUnknown* object);

    namespace Detail {

        /** @brief Return whether @p extensionClass may be attached to @p object under its Extends declarations. */
        [[nodiscard]] bool CanAttachExtension(const BaseUnknown* object, const MetaClass& extensionClass) noexcept;

        /** @brief Resolve @p iid from @p object, retain its anchor on success, and return the facet address. */
        [[nodiscard]] BaseUnknown* QueryInterfaceFacet(BaseUnknown* object, Iid iid,
                                                       BaseUnknown*& retainedAnchor) noexcept;

    } // namespace Detail

    /** @brief Attach @p extension to @p object's nucleus and transfer ownership to the closure. */
    template<class Extension>
        requires Traits::ExtensionClass<Extension>
    void AttachExtension(BaseUnknown* object, ComPtr<Extension>&& extension) {
        if (!object || !extension) {
            return;
        }
        Extension* raw = extension.Get();
        assert(raw->Nucleus() == raw);
        assert(raw->StorageCountForDebug() == 1);
        if (raw->Nucleus() != raw || raw->StorageCountForDebug() != 1) {
            return;
        }
        BaseUnknown* nucleus = object->Nucleus();
        if (!Detail::CanAttachExtension(nucleus, raw->GetMeta())) {
            return;
        }
        raw = extension.Detach();
        raw->BindExtendee(nucleus);
        nucleus->AdoptExtensionNode(raw);
    }

    /** @brief Attach @p facet to @p object's nucleus and transfer ownership to the closure. */
    template<class Facet>
        requires(YObjectClass<Facet> && IsTie(ClassTypeOf<Facet>))
    void AttachBoundFacet(BaseUnknown* object, ComPtr<Facet>&& facet) {
        if (!object || !facet) {
            return;
        }
        Facet* raw = facet.Get();
        assert(raw->StorageCountForDebug() == 1);
        if (raw->StorageCountForDebug() != 1) {
            return;
        }
        if (raw->BoundTarget() && raw->BoundTarget()->Nucleus() != object->Nucleus()) {
            return;
        }
        raw = facet.Detach();
        if (!raw->BoundTarget()) {
            raw->BindBoundTarget(object);
        }
        object->Nucleus()->AdoptBoundFacetNode(raw);
    }

    /** @brief Resolve interface or object facet @p Interface from @p object and return an owning pointer. */
    template<class Interface>
        requires BaseUnknownClass<Interface>
    [[nodiscard]] ComPtr<Interface> QueryInterface(BaseUnknown* object) noexcept {
        BaseUnknown* retainedAnchor = nullptr;
        BaseUnknown* facet = Detail::QueryInterfaceFacet(object, IidOf<Interface>(), retainedAnchor);
        return Detail::ComPtrAccess::AdoptRetained(static_cast<Interface*>(facet), retainedAnchor);
    }

    /** @brief Resolve interface or object facet @p Interface from an owning handle. */
    template<class Interface, class T>
        requires BaseUnknownClass<Interface>
    [[nodiscard]] ComPtr<Interface> QueryInterface(const ComPtr<T>& object) noexcept {
        return QueryInterface<Interface>(object.Anchor());
    }

    /** @brief Return @p object's direct runtime metaclass, or null for a null input. */
    [[nodiscard]] const MetaClass* TypeOf(const BaseUnknown* object) noexcept;

    /** @brief Return whether @p object's closure provides @p iid. */
    [[nodiscard]] bool Provides(const BaseUnknown* object, Iid iid) noexcept;

    /** @brief Return a lifetime-safe snapshot of provider entries visible from @p object's closure. */
    [[nodiscard]] PinnedView<ProviderEntry> IidsOf(const BaseUnknown* object);

    /** @brief Return the class that contributes provider @p iid in @p object's closure. */
    [[nodiscard]] const MetaClass* ProviderClass(const BaseUnknown* object, Iid iid) noexcept;

} // namespace Yuki