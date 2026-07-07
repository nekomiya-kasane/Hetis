/**
 * @file BaseObject.h
 * @brief BaseUnknown lifetime root, closure binding, weak references, and object-model declaration macro.
 * @ingroup Core
 */
#pragma once

#include "Sora/Kernel/Core/IID.h"
#include "Sora/Kernel/Core/MetaClass.h"
#include "Sora/Kernel/Core/Traits.h"
#include "Sora/Core/GetSet.h"

#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace Sora::Kernel {

    // ----------------------------------------------------------------------------------------------------

#define S_OBJECT                                                                                                       \
public:                                                                                                                \
    using Self = typename [:std::meta::access_context::current().scope():];                                            \
    template<size_t I = 0>                                                                                             \
    using Base = Sora::Traits::DirectBaseType<Self, I>;                                                                \
                                                                                                                       \
    [[nodiscard]] Iid GetIid() const noexcept override {                                                               \
        return Sora::Kernel::Traits::IidOf<Self>;                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] static std::shared_ptr<const Sora::Kernel::MetaClass> GetMetaStatic() noexcept {                     \
        return Sora::Kernel::MetaClass::Query<Self>();                                                                 \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] std::shared_ptr<const Sora::Kernel::MetaClass> GetMeta() const noexcept override {                   \
        return GetMetaStatic();                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] static std::string_view GetClassNameStatic() noexcept {                                              \
        return GetMetaStatic()->GetClassName();                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] std::string_view GetClassName() const noexcept override {                                            \
        return GetMeta()->GetClassName();                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] static Sora::Kernel::TypeOfClass GetRoleStatic() noexcept {                                          \
        return Sora::Kernel::Traits::RoleOf<Self>;                                                                     \
    }                                                                                                                  \
                                                                                                                       \
    [[nodiscard]] Sora::Kernel::TypeOfClass GetRole() const noexcept override {                                        \
        return GetRoleStatic();                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    template<class T = Self>                                                                                           \
    [[nodiscard]] static T* Create()                                                                                   \
        requires(Sora::Kernel::IsExtension(Sora::Kernel::Traits::RoleOf<T>))                                           \
    {                                                                                                                  \
        return new T();                                                                                                \
    }                                                                                                                  \
                                                                                                                       \
    ALLOW_GET_SET                                                                                                      \
                                                                                                                       \
public:

    // ----------------------------------------------------------------------------------------------------

    namespace Detail {

        /** @brief Cold closure graph state stored outside the hot BaseUnknown payload. */
        struct ClosureState;

        /** @brief Opaque friend that owns BaseUnknown's storage-level object-model operations. */
        class BaseUnknownInternal;

    } // namespace Detail

    /** @brief Shared state behind weak references to a closure nucleus. */
    struct WeakState {
        std::atomic<BaseUnknown*> nucleus{}; /**< Null when the nucleus has been destroyed. */
    };

    /** @brief Non-owning weak reference to a component closure nucleus. */
    class WeakRef {
    public:
        /** @brief Construct an empty weak reference. */
        WeakRef() noexcept = default;

        /** @brief Construct from shared weak state. */
        explicit WeakRef(std::shared_ptr<WeakState> state) noexcept : state_(std::move(state)) {}

        /** @brief Return whether the referenced nucleus is gone or was never set. */
        [[nodiscard]] bool Expired() const noexcept { return !Get(); }

        /** @brief Return the referenced nucleus, or null after destruction. */
        [[nodiscard]] BaseUnknown* Get() const noexcept {
            return state_ ? state_->nucleus.load(std::memory_order_acquire) : nullptr;
        }

    private:
        std::shared_ptr<WeakState> state_{};
    };

    /** @brief Intrusive lifetime root and type-erased object-model anchor. */
    class alignas(16) [[= Sora::Kernel::$::Role{TypeOfClass::Implementation}]] BaseUnknown {
        /** @brief Compressed BaseUnknown payload: pointer kind, refcount, and one 16-byte-aligned pointer. */
        class ComData {
        public:
            /** @brief Core_Old-style interpretation of the pointer arm stored in BaseUnknownData. */
            enum class PointerType : uint8_t {
                ForImplementation = 0, /**< Pointer arm is the implementation object's cold chain. */
                ForExtension = 1,      /**< Pointer arm is the extended object. */
                ForTie = 2,            /**< Pointer arm is the bound object for a TIE/bound facet. */
                ForInlineFacet = 3,    /**< Pointer arm is the owning provider of an inline facet subobject. */
            };

            /** @brief Refcount value used by external-lifetime objects. */
            static constexpr uint16_t kExternalRefCount = 0xffffu;
            /** @brief Highest incrementable refcount value. */
            static constexpr uint16_t kSaturationLimit = 0xfffeu;

            /** @brief Return the initial implementation payload with one storage reference. */
            [[nodiscard]] static constexpr uint64_t Initial() noexcept { return uint64_t{1} << kRefShift; }

            /** @brief Build a payload from a kind, pointer, and refcount. */
            [[nodiscard]] static uint64_t Make(PointerType kind, const void* pointer, uint16_t refCount) noexcept {
                return static_cast<uint64_t>(kind) | (uint64_t{refCount} << kRefShift) | EncodePointer(pointer);
            }

            /** @brief Return the pointer interpretation kind stored in @p word. */
            [[nodiscard]] static constexpr PointerType Kind(uint64_t word) noexcept {
                return static_cast<PointerType>(word & kKindMask);
            }

            /** @brief Return the refcount stored in @p word. */
            [[nodiscard]] static constexpr uint16_t RefCount(uint64_t word) noexcept {
                return static_cast<uint16_t>((word >> kRefShift) & kRefMaskValue);
            }

            /** @brief Return whether @p word denotes external lifetime. */
            [[nodiscard]] static constexpr bool IsExternalLifetime(uint64_t word) noexcept {
                return RefCount(word) == kExternalRefCount;
            }

            /** @brief Decode the pointer arm stored in @p word. */
            [[nodiscard]] static void* Pointer(uint64_t word) noexcept {
                return std::bit_cast<void*>((word >> kPointerShift) << kPointerAlignmentShift);
            }

            /** @brief Return @p word with the same refcount and a replacement pointer kind and pointer. */
            [[nodiscard]] static uint64_t WithPointer(uint64_t word, PointerType kind, const void* pointer) noexcept {
                return (word & kRefMask) | static_cast<uint64_t>(kind) | EncodePointer(pointer);
            }

            /** @brief Increment @p data unless it is saturated; external lifetime is a no-op success. */
            [[nodiscard]] static bool TryIncrement(std::atomic<uint64_t>& data) noexcept;

            /** @brief Decrement @p data and return true iff the decrement transitioned to zero. */
            [[nodiscard]] static bool TryDecrement(std::atomic<uint64_t>& data) noexcept;

        private:
            static constexpr uint64_t kKindMask = 0x0full;
            static constexpr uint64_t kRefShift = 4;
            static constexpr uint64_t kRefMaskValue = 0xffffull;
            static constexpr uint64_t kRefMask = kRefMaskValue << kRefShift;
            static constexpr uint64_t kPointerShift = 20;
            static constexpr uint64_t kPointerAlignmentShift = 4;
            static constexpr std::uintptr_t kPointerAlignmentMask = (std::uintptr_t{1} << kPointerAlignmentShift) - 1;

            [[nodiscard]] static uint64_t EncodePointer(const void* pointer) noexcept {
                const auto address = reinterpret_cast<std::uintptr_t>(pointer);
                assert((address & kPointerAlignmentMask) == 0);
                return (address >> kPointerAlignmentShift) << kPointerShift;
            }
        };

    public:
        using Self = BaseUnknown;

        /** @brief Construct with an initial storage/strong reference. */
        BaseUnknown() noexcept = default;

        BaseUnknown(const BaseUnknown&) = delete;
        BaseUnknown& operator=(const BaseUnknown&) = delete;

        /** @brief Return the meta-class associated with this object type. */
        [[nodiscard]] static std::shared_ptr<const MetaClass> GetMetaStatic() noexcept {
            return MetaClass::Query<Self>();
        }

        /** @brief Return the meta-class associated with this object. */
        [[nodiscard]] virtual std::shared_ptr<const MetaClass> GetMeta() const noexcept {
            return MetaClass::Query<Self>();
        }

        /** @brief Return the interface ID associated with this object type. */
        [[nodiscard]] static Iid GetIidStatic() noexcept { return Sora::Kernel::Traits::IidOf<Self>; }

        /** @brief Return the interface ID associated with this object. */
        [[nodiscard]] virtual Iid GetIid() const noexcept { return Sora::Kernel::Traits::IidOf<Self>; }

        /** @brief Return the role associated with this object type. */
        [[nodiscard]] static TypeOfClass GetRoleStatic() noexcept { return TypeOfClass::Implementation; }

        /** @brief Return the role associated with this object. */
        [[nodiscard]] virtual TypeOfClass GetRole() const noexcept { return TypeOfClass::Implementation; }

        /** @brief Return the class name associated with this object type. */
        [[nodiscard]] static std::string_view GetClassNameStatic() noexcept { return GetMetaStatic()->GetClassName(); }

        /** @brief Return the class name associated with this object. */
        [[nodiscard]] virtual std::string_view GetClassName() const noexcept { return GetMeta()->GetClassName(); }

        /** @brief Return the closure nucleus controlling lifetime for this object. */
        [[nodiscard]] BaseUnknown* Nucleus() noexcept;

        /** @brief Return the closure nucleus controlling lifetime for this object. */
        [[nodiscard]] const BaseUnknown* Nucleus() const noexcept;

        /** @brief Return the component extended by this extension, or null for non-extensions. */
        [[nodiscard]] BaseUnknown* Extendee() noexcept;

        /** @brief Return the component extended by this extension, or null for non-extensions. */
        [[nodiscard]] const BaseUnknown* Extendee() const noexcept;

        /** @brief Return the object bound by this bound facet, or null for non-bound facets. */
        [[nodiscard]] BaseUnknown* BoundTarget() noexcept;

        /** @brief Return the object bound by this bound facet, or null for non-bound facets. */
        [[nodiscard]] const BaseUnknown* BoundTarget() const noexcept;

        /** @brief Return a weak reference associated with this object's nucleus. */
        [[nodiscard]] WeakRef GetComponentWeakRef();

    protected:
        /** @brief Destroy this nucleus or closure-owned node. Use @ref Release instead of direct deletion. */
        virtual ~BaseUnknown() noexcept;

        friend class Sora::Kernel::Detail::BaseUnknownInternal;

    private:
        template<Concept::ComClass T, class... Args>
        friend ComPtr<T> MakeComPtr(Args&&... args);

        friend void Retain(BaseUnknown* object) noexcept;
        friend void Release(BaseUnknown* object) noexcept;

        /** @brief Add one reference to this object storage or nucleus. */
        void Retain() noexcept;

        /** @brief Release one reference and return true on the last-reference transition. */
        bool Release() noexcept;

        mutable std::atomic<uint64_t> data_{ComData::Initial()};
    };

    static_assert(sizeof(BaseUnknown) == 2 * sizeof(void*));
    static_assert(alignof(BaseUnknown) >= 16);

    namespace Detail {

        /** @brief Internal closure-graph operations intentionally kept out of the public BaseUnknown API. */
        class BaseUnknownInternal {
        public:
            /**
             * @brief Bind @p object as an extension of @p extendee.
             *        Extension --> Extendee
             */
            static void BindExtendee(BaseUnknown* object, BaseUnknown* extendee) noexcept;

            /**
             * @brief Bind @p object as a bound facet of @p target.
             *        BoundFacet --> BoundTarget
             */
            static void BindBoundTarget(BaseUnknown* object, BaseUnknown* target) noexcept;

            /** @brief Adopt @p extension as a closure-owned extension node under @p nucleus. */
            [[nodiscard]] static bool AdoptExtensionNode(BaseUnknown* nucleus, BaseUnknown* extension);

            /** @brief Find an already materialized extension by extension class IID. */
            [[nodiscard]] static BaseUnknown* FindExtensionNode(BaseUnknown* component, Iid extensionIid) noexcept;

            /** @brief Adopt @p facet as a closure-owned bound facet node under @p component. */
            [[nodiscard]] static bool AdoptBoundFacetNode(BaseUnknown* component, Iid interfaceIid, BaseUnknown* facet);

            /** @brief Find an already materialized bound facet for @p interfaceIid. */
            [[nodiscard]] static BaseUnknown* FindBoundFacetNode(BaseUnknown* component, Iid interfaceIid) noexcept;

            /** @brief Return a stable snapshot of closure-owned extension nodes. */
            [[nodiscard]] static std::vector<BaseUnknown*> SnapshotExtensionNodes(BaseUnknown* component);

        private:
            friend class Sora::Kernel::BaseUnknown;

            /** @brief Bind @p object to @p base with an internal pointer-arm kind. */
            static void BindObjectModelBase(BaseUnknown* object, BaseUnknown::ComData::PointerType kind,
                                            BaseUnknown* base) noexcept;

            /** @brief Reset @p object from an object-model pointer arm back to an unbound implementation payload. */
            static void UnbindObjectModelBase(BaseUnknown* object) noexcept;

            /** @brief Remove @p object from @p implementation's cold closure state and reset its object-model base. */
            static void UnbindObjectModelBase(BaseUnknown* implementation, BaseUnknown* object) noexcept;

            /** @brief Return the cold closure state, allocating it on the implementation nucleus when needed. */
            [[nodiscard]] static ClosureState& EnsureClosureState(BaseUnknown* object);

            /** @brief Return the cold closure state if it already exists. */
            [[nodiscard]] static ClosureState* TryClosureState(BaseUnknown* object) noexcept;
        };

    } // namespace Detail

    /** @brief Add one strong reference to @p object's nucleus when it is non-null. */
    void Retain(BaseUnknown* object) noexcept;

    /** @brief Release one strong reference to @p object's nucleus when it is non-null. */
    void Release(BaseUnknown* object) noexcept;

} // namespace Sora::Kernel
