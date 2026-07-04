#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/MetaClass.h"

#include <mutex>

namespace Sora::Kernel {

    namespace Detail {

        /** @brief Cold object chain allocated only when extensions, bound facets, or weak refs are used. */
        struct alignas(16) ClosureState {
            mutable std::mutex mutex{};              /**< Serializes chain mutation and snapshots. */
            std::vector<BaseUnknown*> extensions{};  /**< Closure-owned extension nodes. */
            std::vector<BaseUnknown*> boundFacets{}; /**< Closure-owned bound facet nodes. */
            std::shared_ptr<WeakState> weak{std::make_shared<WeakState>()}; /**< Weak-reference state. */
        };

        class BaseUnknownInternal {
        public:
            using CD = BaseUnknown::ComData;
            using PT = BaseUnknown::ComData::PointerType;

            static void DestroyClosureState(ClosureState* state) noexcept {
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
                    Release(facet);
                }
                for (BaseUnknown* extension : extensions) {
                    Release(extension);
                }
                delete state;
            }

            static void StorePointerKind(PT kind, BaseUnknown* pointer) noexcept {
                for (uint64_t current = pointer->data_.load(std::memory_order_acquire);;) {
                    const auto currentKind = CD::Kind(current);
                    assert(currentKind != PT::ForImplementation || CD::Pointer(current) == nullptr);
                    const uint64_t next = CD::WithPointer(current, kind, pointer);
                    if (pointer->data_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                        return;
                    }
                }
            }

            static Detail::ClosureState& EnsureClosureState(BaseUnknown* pointer) {
                const BaseUnknown* nucleus = pointer->Nucleus();
                if (nucleus != pointer) {
                    return EnsureClosureState(const_cast<BaseUnknown*>(nucleus));
                }

                Detail::ClosureState* candidate = nullptr;

                for (uint64_t current = pointer->data_.load(std::memory_order_acquire);;) {
                    assert(CD::Kind(current) == PT::ForImplementation);
                    if (auto* existing = static_cast<Detail::ClosureState*>(CD::Pointer(current))) {
                        return *existing;
                    }

                    if (!candidate) {
                        candidate = new Detail::ClosureState;
                        candidate->weak->nucleus.store(const_cast<BaseUnknown*>(pointer), std::memory_order_release);
                    }

                    const uint64_t next = CD::WithPointer(current, PT::ForImplementation, candidate);
                    if (pointer->data_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                        return *candidate;
                    }
                }
            }

            static Detail::ClosureState* TryClosureState(BaseUnknown* pointer) noexcept {
                const BaseUnknown* nucleus = pointer->Nucleus();
                if (nucleus != pointer) {
                    return TryClosureState(const_cast<BaseUnknown*>(nucleus));
                }

                const uint64_t word = pointer->data_.load(std::memory_order_acquire);
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
        Detail::BaseUnknownInternal::DestroyClosureState(static_cast<Detail::ClosureState*>(ComData::Pointer(word)));
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
            assert(false && "Extendee() called on a non-extension object");
            return nullptr;
        }
        return static_cast<BaseUnknown*>(ComData::Pointer(word));
    }

    const BaseUnknown* BaseUnknown::Extendee() const noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        if (ComData::Kind(word) != ComData::PointerType::ForExtension) {
            assert(false && "Extendee() called on a non-extension object");
            return nullptr;
        }
        return static_cast<const BaseUnknown*>(ComData::Pointer(word));
    }

    BaseUnknown* BaseUnknown::BoundTarget() noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        if (ComData::Kind(word) != ComData::PointerType::ForTie &&
            ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
            assert(false && "BoundTarget() called on a non-tie object");
            return nullptr;
        }
        return static_cast<BaseUnknown*>(ComData::Pointer(word));
    }

    const BaseUnknown* BaseUnknown::BoundTarget() const noexcept {
        const uint64_t word = data_.load(std::memory_order_acquire);
        if (ComData::Kind(word) != ComData::PointerType::ForTie &&
            ComData::Kind(word) != ComData::PointerType::ForInlineFacet) {
            assert(false && "BoundTarget() called on a non-tie object");
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
        (void)object->Release();
    }

} // namespace Sora::Kernel