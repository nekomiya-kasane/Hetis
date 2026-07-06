#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include "Sora/Kernel/Core/IID.h"
#include "Sora/Kernel/Core/MetaClass.h"

#include "Kernel/Core/ObjectModelInternal.h"

#include <algorithm>
#include <mutex>
#include <flat_map>

namespace Sora::Kernel {

    namespace Detail {

        /** @brief Cold object chain allocated only when extensions, bound facets, or weak refs are used. */
        struct alignas(16) ClosureState {
            mutable std::mutex mutex{};                     /**< Serializes chain mutation and snapshots. */
            std::flat_map<Iid, BaseUnknown*> extensions{};  /**< Closure-owned extension nodes. */
            std::flat_map<Iid, BaseUnknown*> boundFacets{}; /**< Closure-owned bound facet nodes. */
            std::shared_ptr<WeakState> weak{std::make_shared<WeakState>()}; /**< Weak-reference state. */

            ~ClosureState() noexcept {
                if (weak) {
                    weak->nucleus.store(nullptr, std::memory_order_release);
                }

                {
                    std::scoped_lock lock(mutex);
                    for (const auto& extension : extensions | std::views::values) {
                        Release(extension);
                    }
                    for (const auto& facet : boundFacets | std::views::values) {
                        Release(facet);
                    }
                    extensions.clear();
                    boundFacets.clear();
                }
            }

            void SetExtension(Iid iid, BaseUnknown* extension) {
                std::scoped_lock lock(mutex);
                assert(!extensions.contains(iid) || extensions[iid] == extension);
                extensions[iid] = extension;
            }

            void SetBoundFacet(Iid iid, BaseUnknown* facet) {
                std::scoped_lock lock(mutex);
                assert(!boundFacets.contains(iid) || boundFacets[iid] == facet);
                boundFacets[iid] = facet;
            }

            void InvalidateWeakRef() noexcept {
                std::scoped_lock lock(mutex);
                weak->nucleus.store(nullptr, std::memory_order_release);
            }
        };

        /** @brief Internal closure-graph operations intentionally kept out of the public BaseUnknown API. */
        class BaseUnknownInternal {
        public:
            /**
             * @brief Bind @p object as an extension of @p extendee.
             *        Extension --> Extendee
             */
            static void BindExtendee(BaseUnknown* object, BaseUnknown* extendee) noexcept {
                if (!object || !extendee) {
                    return;
                }
                BindObjectModelBase(object, BaseUnknown::ComData::PointerType::ForExtension, extendee->Nucleus());
            }

            /**
             * @brief Bind @p object as a bound facet of @p target.
             *        BoundFacet --> BoundTarget
             */
            static void BindBoundTarget(BaseUnknown* object, BaseUnknown* target) noexcept {
                if (!object || !target) {
                    return;
                }
                BindObjectModelBase(object, BaseUnknown::ComData::PointerType::ForTie, target);
            }

            /** @brief Adopt @p extension as a closure-owned extension node under @p nucleus. */
            static bool AdoptExtensionNode(BaseUnknown* nucleus, BaseUnknown* extension) {
                if (!nucleus || !extension) {
                    return false;
                }
                assert(IsExtension(extension->GetRole()) && !extension->Extendee());

                nucleus = nucleus->Nucleus();
                Detail::ClosureState& state = EnsureClosureState(nucleus);

                state.SetExtension(extension->GetIid(), extension);
                BindObjectModelBase(extension, BaseUnknown::ComData::PointerType::ForExtension, nucleus);

                return true;
            }

            /** @brief Adopt @p facet as a closure-owned bound facet node under @p component. */
            [[nodiscard]] static bool AdoptBoundFacetNode(BaseUnknown* component, Iid interfaceIid,
                                                          BaseUnknown* facet) {
                if (!component || !facet) {
                    return false;
                }
                assert(IsTie(facet->GetRole()) && !facet->BoundTarget());

                Detail::ClosureState& state = EnsureClosureState(component->Nucleus());
                std::scoped_lock lock(state.mutex);
                if (auto existing = state.boundFacets.find(interfaceIid); existing != state.boundFacets.end()) {
                    return false;
                }

                state.SetBoundFacet(interfaceIid, facet);
                BindObjectModelBase(facet, BaseUnknown::ComData::PointerType::ForTie, component);
                return true;
            }

        private:
            friend class Sora::Kernel::BaseUnknown;

            /** @brief Bind @p object to @p base with an internal pointer-arm kind. */
            static void BindObjectModelBase(BaseUnknown* object, BaseUnknown::ComData::PointerType kind,
                                            BaseUnknown* base) noexcept {
                using PT = BaseUnknown::ComData::PointerType;

                assert(object != nullptr);
                assert(base != nullptr);
                assert(kind == PT::ForExtension || kind == PT::ForTie || kind == PT::ForInlineFacet);
                for (uint64_t current = object->data_.load(std::memory_order_acquire);;) {
                    assert(BaseUnknown::ComData::Kind(current) == PT::ForImplementation &&
                           BaseUnknown::ComData::Pointer(current) == nullptr);
                    const uint64_t next = BaseUnknown::ComData::WithPointer(current, kind, base);
                    if (object->data_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                        return;
                    }
                }
            }

            /** @brief Reset @p object from an object-model pointer arm back to an unbound implementation payload. */
            static void UnbindObjectModelBase(BaseUnknown* object) noexcept {
                using PT = BaseUnknown::ComData::PointerType;

                if (!object) {
                    return;
                }
                for (uint64_t current = object->data_.load(std::memory_order_acquire);;) {
                    const PT kind = BaseUnknown::ComData::Kind(current);
                    if (kind == PT::ForImplementation) {
                        return;
                    }
                    assert(kind == PT::ForExtension || kind == PT::ForTie || kind == PT::ForInlineFacet);
                    const uint64_t next = BaseUnknown::ComData::WithPointer(current, PT::ForImplementation, nullptr);
                    if (object->data_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                        return;
                    }
                }
            }

            /** @brief Remove @p object from @p implementation's cold closure state and reset its object-model base. */
            static void UnbindObjectModelBase(BaseUnknown* implementation, BaseUnknown* object) noexcept {
                if (!implementation || !object) {
                    return;
                }

                implementation = implementation->Nucleus();
                if (!implementation) {
                    return;
                }
                Detail::ClosureState* state = TryClosureState(implementation);
                if (!state) {
                    return;
                }

                bool removed = false;
                {
                    std::scoped_lock lock(state->mutex);
                    auto eraseObject = [object](std::flat_map<Iid, BaseUnknown*>& nodes) noexcept {
                        const auto found = std::ranges::find_if(
                            nodes, [object](const auto& entry) noexcept { return entry.second == object; });
                        if (found == nodes.end()) {
                            return false;
                        }
                        nodes.erase(found);
                        return true;
                    };

                    if (IsExtension(object->GetRole())) {
                        removed = eraseObject(state->extensions);
                    } else if (IsTie(object->GetRole())) {
                        removed = eraseObject(state->boundFacets);
                    }
                }

                if (removed) {
                    UnbindObjectModelBase(object);
                }
            }

            /** @brief Return the cold closure state, allocating it on the implementation nucleus when needed. */
            static ClosureState& EnsureClosureState(BaseUnknown* object) {
                using CD = BaseUnknown::ComData;
                using PT = BaseUnknown::ComData::PointerType;

                const BaseUnknown* nucleus = object->Nucleus();
                if (nucleus != object) {
                    return EnsureClosureState(const_cast<BaseUnknown*>(nucleus));
                }

                Detail::ClosureState* candidate = nullptr;

                for (uint64_t current = object->data_.load(std::memory_order_acquire);;) {
                    assert(CD::Kind(current) == PT::ForImplementation);
                    if (auto* existing = static_cast<Detail::ClosureState*>(CD::Pointer(current))) {
                        delete candidate;
                        return *existing;
                    }

                    if (!candidate) {
                        candidate = new Detail::ClosureState;
                        candidate->weak->nucleus.store(const_cast<BaseUnknown*>(object), std::memory_order_release);
                    }

                    const uint64_t next = CD::WithPointer(current, PT::ForImplementation, candidate);
                    if (object->data_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                        return *candidate;
                    }
                }
            }

            /** @brief Return the cold closure state if it already exists. */
            [[nodiscard]] static ClosureState* TryClosureState(BaseUnknown* object) noexcept {
                using CD = BaseUnknown::ComData;
                using PT = BaseUnknown::ComData::PointerType;

                const BaseUnknown* nucleus = object->Nucleus();
                if (nucleus != object) {
                    return TryClosureState(const_cast<BaseUnknown*>(nucleus));
                }

                const uint64_t word = object->data_.load(std::memory_order_acquire);
                if (CD::Kind(word) != PT::ForImplementation) {
                    return nullptr;
                }
                return static_cast<Detail::ClosureState*>(CD::Pointer(word));
            }
        };

    } // namespace Detail

    bool BaseUnknown::ComData::TryIncrement(std::atomic<uint64_t>& data) noexcept {
        for (uint64_t current = data.load(std::memory_order_relaxed);;) {
            if (IsExternalLifetime(current)) {
                return true;
            }
            const uint16_t refCount = RefCount(current);
            if (refCount >= kSaturationLimit) {
                return false;
            }
            const uint64_t next = (current & ~kRefMask) | (uint64_t{static_cast<uint16_t>(refCount + 1)} << kRefShift);
            if (data.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    bool BaseUnknown::ComData::TryDecrement(std::atomic<uint64_t>& data) noexcept {
        for (uint64_t current = data.load(std::memory_order_relaxed);;) {
            if (IsExternalLifetime(current)) {
                return false;
            }
            const uint16_t refCount = RefCount(current);
            if (refCount == 0) {
                return false;
            }
            const uint64_t next = (current & ~kRefMask) | (uint64_t{static_cast<uint16_t>(refCount - 1)} << kRefShift);
            if (data.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return refCount == 1;
            }
        }
    }

    BaseUnknown::~BaseUnknown() noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        if (ComData::Kind(word) != ComData::PointerType::ForImplementation) {
            return;
        }
        auto* state = static_cast<Detail::ClosureState*>(ComData::Pointer(word));
        delete state;
    }

    BaseUnknown* BaseUnknown::Nucleus() noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        const auto kind = ComData::Kind(word);
        if (kind == ComData::PointerType::ForImplementation) {
            return this;
        }
        auto* owner = static_cast<BaseUnknown*>(ComData::Pointer(word));
        return owner ? owner->Nucleus() : nullptr;
    }

    const BaseUnknown* BaseUnknown::Nucleus() const noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        const auto kind = ComData::Kind(word);
        if (kind == ComData::PointerType::ForImplementation) {
            return this;
        }
        auto* owner = static_cast<const BaseUnknown*>(ComData::Pointer(word));
        return owner ? owner->Nucleus() : nullptr;
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
        if (ComData::Kind(word) != ComData::PointerType::ForTie &&
            ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
            return nullptr;
        }
        return static_cast<BaseUnknown*>(ComData::Pointer(word));
    }

    const BaseUnknown* BaseUnknown::BoundTarget() const noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        if (ComData::Kind(word) != ComData::PointerType::ForTie &&
            ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
            return nullptr;
        }
        return static_cast<const BaseUnknown*>(ComData::Pointer(word));
    }

    WeakRef BaseUnknown::GetComponentWeakRef() {
        BaseUnknown* nucleus = Nucleus();
        Detail::ClosureState& state = Detail::BaseUnknownInternal::EnsureClosureState(nucleus);
        state.weak->nucleus.store(nucleus, std::memory_order_release);
        return WeakRef{state.weak};
    }

    void BaseUnknown::Retain() noexcept {
        if (!ComData::TryIncrement(data_)) {
            assert(false && "Retain() called on a saturated or external-lifetime object");
        }
    }

    bool BaseUnknown::Release() noexcept {
        if (ComData::TryDecrement(data_)) {
            delete this;
            return true;
        }
        return false;
    }

    void Retain(BaseUnknown* object) noexcept {
        if (!object) {
            return;
        }
        object->Retain();
    }

    void Release(BaseUnknown* object) noexcept {
        if (!object) {
            return;
        }
        object->Release();
    }

} // namespace Sora::Kernel
