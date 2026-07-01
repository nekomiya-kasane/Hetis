/**
 * @file BaseUnknown.cpp
 * @brief Compact intrusive lifetime and closure-state implementation.
 * @ingroup Core
 */
#include "Yuki/Core/Meta.h"
#include "Yuki/Core/BaseUnknown.h"

namespace Yuki {

BaseUnknown::~BaseUnknown() noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForImplementation) {
        return;
    }
    DestroyClosureState(static_cast<Detail::ClosureState*>(ComData::Pointer(word)));
}

const MetaClass& BaseUnknown::GetMeta() const noexcept {
    return BaseUnknownMetaClass();
}

BaseUnknown* BaseUnknown::Nucleus() noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    const auto kind = ComData::Kind(word);
    if (kind == ComData::PointerType::ForExtension || kind == ComData::PointerType::ForTie ||
        kind == ComData::PointerType::ForInlineFacet) {
        auto* owner = static_cast<BaseUnknown*>(ComData::Pointer(word));
        return owner ? owner->Nucleus() : this;
    }
    return this;
}

const BaseUnknown* BaseUnknown::Nucleus() const noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    const auto kind = ComData::Kind(word);
    if (kind == ComData::PointerType::ForExtension || kind == ComData::PointerType::ForTie ||
        kind == ComData::PointerType::ForInlineFacet) {
        auto* owner = static_cast<const BaseUnknown*>(ComData::Pointer(word));
        return owner ? owner->Nucleus() : this;
    }
    return this;
}

BaseUnknown* BaseUnknown::Extendee() noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForExtension) {
        return nullptr;
    }
    return static_cast<BaseUnknown*>(ComData::Pointer(word));
}

const BaseUnknown* BaseUnknown::Extendee() const noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForExtension) {
        return nullptr;
    }
    return static_cast<const BaseUnknown*>(ComData::Pointer(word));
}

BaseUnknown* BaseUnknown::BoundTarget() noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForTie) {
        return nullptr;
    }
    return static_cast<BaseUnknown*>(ComData::Pointer(word));
}

const BaseUnknown* BaseUnknown::BoundTarget() const noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForTie) {
        return nullptr;
    }
    return static_cast<const BaseUnknown*>(ComData::Pointer(word));
}

BaseUnknown* BaseUnknown::InlineFacetAnchor() noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
        return nullptr;
    }
    return static_cast<BaseUnknown*>(ComData::Pointer(word));
}

const BaseUnknown* BaseUnknown::InlineFacetAnchor() const noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
        return nullptr;
    }
    return static_cast<const BaseUnknown*>(ComData::Pointer(word));
}

void BaseUnknown::BindExtendee(BaseUnknown* extendee) noexcept {
    StorePointerKind(ComData::PointerType::ForExtension, extendee ? extendee->Nucleus() : nullptr);
}

void BaseUnknown::BindBoundTarget(BaseUnknown* target) noexcept {
    StorePointerKind(ComData::PointerType::ForTie, target ? target->Nucleus() : nullptr);
}

void BaseUnknown::BindInlineFacetAnchor(BaseUnknown* anchor) noexcept {
    StorePointerKind(ComData::PointerType::ForInlineFacet, anchor ? anchor->Nucleus() : nullptr);
}

void BaseUnknown::StorePointerKind(ComData::PointerType kind, BaseUnknown* pointer) noexcept {
    for (uint64_t current = data_.load(std::memory_order_acquire);;) {
        const auto currentKind = ComData::Kind(current);
        assert(currentKind != ComData::PointerType::ForImplementation || ComData::Pointer(current) == nullptr);
        const uint64_t next = ComData::WithPointer(current, kind, pointer);
        if (data_.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void BaseUnknown::DestroyClosureState(Detail::ClosureState* state) noexcept {
    if (!state) {
        return;
    }

    if (state->weak) {
        state->weak->nucleus.store(nullptr, std::memory_order_release);
    }

    std::vector<BaseUnknown*> extensions;
    std::vector<BaseUnknown*> facets;
    {
        std::scoped_lock lock(state->mutex);
        extensions = std::move(state->extensions);
        facets = std::move(state->boundFacets);
    }

    for (BaseUnknown* facet : facets) {
        ReleaseStorageReference(facet);
    }
    for (BaseUnknown* extension : extensions) {
        ReleaseStorageReference(extension);
    }
    delete state;
}

void BaseUnknown::Require() noexcept {
    const bool retained = ComData::TryIncrement(data_);
    assert(retained);
}

bool BaseUnknown::Release() noexcept {
    return ComData::TryDecrement(data_);
}

void BaseUnknown::ReleaseStorageReference(BaseUnknown* object) noexcept {
    if (object && object->Release()) {
        delete object;
    }
}

uint32_t BaseUnknown::StrongCountForDebug() const noexcept {
    const BaseUnknown* nucleus = Nucleus();
    const uint64_t word = nucleus->data_.load(std::memory_order_acquire);
    return ComData::RefCount(word);
}

uint32_t BaseUnknown::StorageCountForDebug() const noexcept {
    const uint64_t word = data_.load(std::memory_order_acquire);
    return ComData::RefCount(word);
}

Detail::ClosureState& BaseUnknown::EnsureClosureState() const {
    const BaseUnknown* nucleus = Nucleus();
    if (nucleus != this) {
        return const_cast<BaseUnknown*>(nucleus)->EnsureClosureState();
    }

    auto* candidate = new Detail::ClosureState;
    candidate->weak->nucleus.store(const_cast<BaseUnknown*>(this), std::memory_order_release);

    for (uint64_t current = data_.load(std::memory_order_acquire);;) {
        assert(ComData::Kind(current) == ComData::PointerType::ForImplementation);
        if (auto* existing = static_cast<Detail::ClosureState*>(ComData::Pointer(current))) {
            delete candidate;
            return *existing;
        }

        const uint64_t next = ComData::WithPointer(current, ComData::PointerType::ForImplementation, candidate);
        if (data_.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return *candidate;
        }
    }
}

Detail::ClosureState* BaseUnknown::TryClosureState() const noexcept {
    const BaseUnknown* nucleus = Nucleus();
    if (nucleus != this) {
        return const_cast<BaseUnknown*>(nucleus)->TryClosureState();
    }

    const uint64_t word = data_.load(std::memory_order_acquire);
    if (ComData::Kind(word) != ComData::PointerType::ForImplementation) {
        return nullptr;
    }
    return static_cast<Detail::ClosureState*>(ComData::Pointer(word));
}

WeakRef BaseUnknown::GetComponentWeakRef() {
    BaseUnknown* nucleus = Nucleus();
    Detail::ClosureState& state = nucleus->EnsureClosureState();
    state.weak->nucleus.store(nucleus, std::memory_order_release);
    return WeakRef{state.weak};
}

void BaseUnknown::AdoptExtensionNode(BaseUnknown* extension) {
    if (!extension) {
        return;
    }
    BaseUnknown* nucleus = Nucleus();
    Detail::ClosureState& state = nucleus->EnsureClosureState();
    std::scoped_lock lock(state.mutex);
    state.extensions.push_back(extension);
}

void BaseUnknown::AdoptBoundFacetNode(BaseUnknown* facet) {
    if (!facet) {
        return;
    }
    BaseUnknown* nucleus = Nucleus();
    Detail::ClosureState& state = nucleus->EnsureClosureState();
    std::scoped_lock lock(state.mutex);
    state.boundFacets.push_back(facet);
}

std::vector<BaseUnknown*> BaseUnknown::CopyExtensionNodes() const {
    const Detail::ClosureState* state = TryClosureState();
    if (!state) {
        return {};
    }
    std::scoped_lock lock(state->mutex);
    return state->extensions;
}

std::vector<BaseUnknown*> BaseUnknown::CopyBoundFacetNodes() const {
    const Detail::ClosureState* state = TryClosureState();
    if (!state) {
        return {};
    }
    std::scoped_lock lock(state->mutex);
    return state->boundFacets;
}

void Retain(BaseUnknown* object) noexcept {
    if (object) {
        object->Nucleus()->Require();
    }
}

void Release(BaseUnknown* object) noexcept {
    if (!object) {
        return;
    }
    BaseUnknown* nucleus = object->Nucleus();
    if (nucleus->Release()) {
        delete nucleus;
    }
}

} // namespace Yuki
