/**
 * @file BaseUnknown.h
 * @brief Compact intrusive lifetime root, closure state, weak references, and object-role concepts.
 * @ingroup Core
 */
#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <cassert>
#include <mutex>

#include "Yuki/Core/Types.h"

namespace Yuki {

    template<class T>
        requires Detail::BaseUnknownRawClass<T>
    class ComPtr;

    class MetaClass;

    /** @brief Return the static metaclass for a reflected object-model class. */
    template<class Self>
    [[nodiscard]] const MetaClass& StaticMetaClass() noexcept;

    /** @brief Return the canonical metaclass for the BaseUnknown root anchor. */
    [[nodiscard]] const MetaClass& BaseUnknownMetaClass() noexcept;

    /** @brief Shared state behind weak references to a closure nucleus. */
    struct WeakState {
        std::atomic<BaseUnknown*> nucleus{}; /**< Null when the nucleus has been destroyed. */
    };

    namespace Detail {

        /** @brief Cold object chain allocated only when extensions, bound facets, or weak refs are used. */
        struct alignas(16) ClosureState {
            mutable std::mutex mutex{};              /**< Serializes chain mutation and snapshots. */
            std::vector<BaseUnknown*> extensions{};  /**< Closure-owned extension nodes. */
            std::vector<BaseUnknown*> boundFacets{}; /**< Closure-owned bound facet nodes. */
            std::shared_ptr<WeakState> weak{std::make_shared<WeakState>()}; /**< Weak-reference state. */
        };

    } // namespace Detail

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
    class alignas(16) BaseUnknown {
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
                return reinterpret_cast<void*>((word >> kPointerShift) << kPointerAlignmentShift);
            }

            /** @brief Return @p word with the same refcount and a replacement pointer kind and pointer. */
            [[nodiscard]] static uint64_t WithPointer(uint64_t word, PointerType kind, const void* pointer) noexcept {
                return (word & kRefMask) | static_cast<uint64_t>(kind) | EncodePointer(pointer);
            }

            /** @brief Increment @p data unless it is saturated; external lifetime is a no-op success. */
            [[nodiscard]] static bool TryIncrement(std::atomic<uint64_t>& data) noexcept {
                for (uint64_t current = data.load(std::memory_order_relaxed);;) {
                    if (IsExternalLifetime(current)) {
                        return true;
                    }
                    const uint16_t refCount = RefCount(current);
                    if (refCount >= kSaturationLimit) {
                        return false;
                    }
                    const uint64_t next =
                        (current & ~kRefMask) | (uint64_t{static_cast<uint16_t>(refCount + 1)} << kRefShift);
                    if (data.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                        return true;
                    }
                }
            }

            /** @brief Decrement @p data and return true iff the decrement transitioned to zero. */
            [[nodiscard]] static bool TryDecrement(std::atomic<uint64_t>& data) noexcept {
                for (uint64_t current = data.load(std::memory_order_relaxed);;) {
                    if (IsExternalLifetime(current)) {
                        return false;
                    }
                    const uint16_t refCount = RefCount(current);
                    if (refCount == 0) {
                        return false;
                    }
                    const uint64_t next =
                        (current & ~kRefMask) | (uint64_t{static_cast<uint16_t>(refCount - 1)} << kRefShift);
                    if (data.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                        return refCount == 1;
                    }
                }
            }

        private:
            static constexpr uint64_t kKindMask = 0x0full;
            static constexpr uint64_t kRefShift = 4;
            static constexpr uint64_t kRefMaskValue = 0xffffull;
            static constexpr uint64_t kRefMask = kRefMaskValue << kRefShift;
            static constexpr uint64_t kPointerShift = 20;
            static constexpr uint64_t kPointerAlignmentShift = 4;
            static constexpr std::uintptr_t kPointerAlignmentMask =
                (std::uintptr_t{1} << kPointerAlignmentShift) - 1;

            [[nodiscard]] static uint64_t EncodePointer(const void* pointer) noexcept {
                const auto address = reinterpret_cast<std::uintptr_t>(pointer);
                assert((address & kPointerAlignmentMask) == 0);
                return (address >> kPointerAlignmentShift) << kPointerShift;
            }
        };

    public:
        /** @brief Construct with an initial storage/strong reference. */
        BaseUnknown() noexcept = default;

        BaseUnknown(const BaseUnknown&) = delete;
        BaseUnknown& operator=(const BaseUnknown&) = delete;

        /** @brief Return the runtime metaclass of this object. */
        [[nodiscard]] virtual const MetaClass& GetMeta() const noexcept;

        /** @brief Return the closure nucleus controlling lifetime for this object. */
        [[nodiscard]] BaseUnknown* Nucleus() noexcept;

        /** @brief Return the closure nucleus controlling lifetime for this object. */
        [[nodiscard]] const BaseUnknown* Nucleus() const noexcept;

        /** @brief Return the complete provider object address for compatibility with old call sites. */
        [[nodiscard]] void* ObjectAddress() noexcept { return this; }

        /** @brief Return the complete provider object address for compatibility with old call sites. */
        [[nodiscard]] const void* ObjectAddress() const noexcept { return this; }

        /** @brief Return the component extended by this extension, or null for non-extensions. */
        [[nodiscard]] BaseUnknown* Extendee() noexcept;

        /** @brief Return the component extended by this extension, or null for non-extensions. */
        [[nodiscard]] const BaseUnknown* Extendee() const noexcept;

        /** @brief Return the object bound by this bound facet, or null for non-bound facets. */
        [[nodiscard]] BaseUnknown* BoundTarget() noexcept;

        /** @brief Return the object bound by this bound facet, or null for non-bound facets. */
        [[nodiscard]] const BaseUnknown* BoundTarget() const noexcept;

        /** @brief Return the closure anchor that owns this inline facet, or null for non-inline facets. */
        [[nodiscard]] BaseUnknown* InlineFacetAnchor() noexcept;

        /** @brief Return the closure anchor that owns this inline facet, or null for non-inline facets. */
        [[nodiscard]] const BaseUnknown* InlineFacetAnchor() const noexcept;

        /** @brief Bind this object as an extension of @p extendee. */
        void BindExtendee(BaseUnknown* extendee) noexcept;

        /** @brief Bind this object as a bound facet of @p target. */
        void BindBoundTarget(BaseUnknown* target) noexcept;

        /** @brief Bind this object as an inline facet subobject anchored by @p anchor. */
        void BindInlineFacetAnchor(BaseUnknown* anchor) noexcept;

        /** @brief Return the current strong count of the nucleus for tests and diagnostics. */
        [[nodiscard]] uint32_t StrongCountForDebug() const noexcept;

        /** @brief Return this object's own storage reference count for tests and attachment checks. */
        [[nodiscard]] uint32_t StorageCountForDebug() const noexcept;

        /** @brief Return a weak reference associated with this object's nucleus. */
        [[nodiscard]] WeakRef GetComponentWeakRef();

        /** @brief Adopt @p extension as a closure-owned extension node. */
        void AdoptExtensionNode(BaseUnknown* extension);

        /** @brief Adopt @p facet as a closure-owned bound facet node. */
        void AdoptBoundFacetNode(BaseUnknown* facet);

        /** @brief Copy the current closure-owned extension node list. */
        [[nodiscard]] std::vector<BaseUnknown*> CopyExtensionNodes() const;

        /** @brief Copy the current closure-owned bound facet node list. */
        [[nodiscard]] std::vector<BaseUnknown*> CopyBoundFacetNodes() const;

        /** @brief Visit closure-owned extension nodes until @p visitor returns true. */
        template<class Visitor>
        bool VisitExtensionNodes(Visitor&& visitor) const {
            const Detail::ClosureState* state = TryClosureState();
            if (!state) {
                return false;
            }
            std::scoped_lock lock(state->mutex);
            for (BaseUnknown* extension : state->extensions) {
                if (extension && visitor(extension)) {
                    return true;
                }
            }
            return false;
        }

        /** @brief Visit closure-owned bound facet nodes until @p visitor returns true. */
        template<class Visitor>
        bool VisitBoundFacetNodes(Visitor&& visitor) const {
            const Detail::ClosureState* state = TryClosureState();
            if (!state) {
                return false;
            }
            std::scoped_lock lock(state->mutex);
            for (BaseUnknown* facet : state->boundFacets) {
                if (facet && visitor(facet)) {
                    return true;
                }
            }
            return false;
        }

    protected:
        /** @brief Destroy this nucleus or closure-owned node. Use @ref Release instead of direct deletion. */
        virtual ~BaseUnknown() noexcept;

    private:
        template<class T, class... Args>
            requires Detail::BaseUnknownRawClass<T>
        friend ComPtr<T> MakeOwned(Args&&... args);
        friend void Retain(BaseUnknown* object) noexcept;
        friend void Release(BaseUnknown* object) noexcept;

        /** @brief Add one reference to this object storage or nucleus. */
        void Require() noexcept;

        /** @brief Release one reference and return true on the last-reference transition. */
        [[nodiscard]] bool Release() noexcept;

        /** @brief Release a closure-owned node's storage reference without following its nucleus relation. */
        static void ReleaseStorageReference(BaseUnknown* object) noexcept;

        /** @brief Destroy a cold object chain and release its closure-owned nodes. */
        static void DestroyClosureState(Detail::ClosureState* state) noexcept;

        /** @brief Replace the pointer-kind arm while preserving the storage refcount. */
        void StorePointerKind(ComData::PointerType kind, BaseUnknown* pointer) noexcept;

        [[nodiscard]] Detail::ClosureState& EnsureClosureState() const;
        [[nodiscard]] Detail::ClosureState* TryClosureState() const noexcept;

        mutable std::atomic<uint64_t> data_{ComData::Initial()};
    };

    static_assert(sizeof(BaseUnknown) == 2 * sizeof(void*));
    static_assert(alignof(BaseUnknown) >= 16);

    /** @brief Add one strong reference to @p object's nucleus when it is non-null. */
    void Retain(BaseUnknown* object) noexcept;

    /** @brief Release one strong reference to @p object's nucleus when it is non-null. */
    void Release(BaseUnknown* object) noexcept;

    namespace Traits {

        /** @brief A complete type that participates in BaseUnknown lifetime and QueryInterface. */
        template<class T>
        concept BaseUnknownClass = Detail::BaseUnknownRawClass<T>;

        /** @brief A type whose declaration is a Yuki interface contract. */
        template<class T>
        concept InterfaceClass = BaseUnknownClass<T> && ClassTypeOf<T> == TypeOfClass::Interface &&
                                 Mashiro::Traits::BasesCount<T> <= 1;

        /** @brief A BaseUnknown-derived class with a direct non-interface object-model role. */
        template<class T>
        concept YObjectClass = BaseUnknownClass<T> && ClassTypeOf<T> != TypeOfClass::NothingType &&
                               ClassTypeOf<T> != TypeOfClass::Interface && Mashiro::Traits::BasesCount<T> <= 1;

        /** @brief A BaseUnknown class declared as a component implementation. */
        template<class T>
        concept ImplementationClass = YObjectClass<T> && ClassTypeOf<T> == TypeOfClass::Implementation;

        /** @brief A BaseUnknown class classified as a stateful extension. */
        template<class T>
        concept DataExtensionClass = YObjectClass<T> && ClassTypeOf<T> == TypeOfClass::DataExtension;

        /** @brief A BaseUnknown class classified as a stateless extension. */
        template<class T>
        concept CodeExtensionClass = YObjectClass<T> && ClassTypeOf<T> == TypeOfClass::CodeExtension;

        /** @brief A BaseUnknown class declared as any extension role. */
        template<class T>
        concept ExtensionClass = DataExtensionClass<T> || CodeExtensionClass<T> ||
                                 (YObjectClass<T> && (ClassTypeOf<T> == TypeOfClass::CacheExtension ||
                                                      ClassTypeOf<T> == TypeOfClass::TransientExtension));

        /** @brief A concrete provider role, either implementation, extension, or TIE/TIEchain. */
        template<class T>
        concept ComponentClass =
            ImplementationClass<T> || ExtensionClass<T> || (YObjectClass<T> && IsTie(ClassTypeOf<T>));

    } // namespace Traits

    /** @brief Namespace-level facade for @ref Traits::BaseUnknownClass. */
    template<class T>
    concept BaseUnknownClass = Traits::BaseUnknownClass<T>;

    /** @brief Namespace-level facade for @ref Traits::InterfaceClass. */
    template<class T>
    concept InterfaceClass = Traits::InterfaceClass<T>;

    /** @brief Namespace-level facade for @ref Traits::YObjectClass. */
    template<class T>
    concept YObjectClass = Traits::YObjectClass<T>;

    /** @brief Namespace-level facade for @ref Traits::ImplementationClass. */
    template<class T>
    concept ImplementationClass = Traits::ImplementationClass<T>;

    /** @brief Namespace-level facade for @ref Traits::DataExtensionClass. */
    template<class T>
    concept DataExtensionClass = Traits::DataExtensionClass<T>;

    /** @brief Namespace-level facade for @ref Traits::CodeExtensionClass. */
    template<class T>
    concept CodeExtensionClass = Traits::CodeExtensionClass<T>;

    /** @brief Namespace-level facade for @ref Traits::ExtensionClass. */
    template<class T>
    concept ExtensionClass = Traits::ExtensionClass<T>;

    /** @brief Namespace-level facade for @ref Traits::ComponentClass. */
    template<class T>
    concept ComponentClass = Traits::ComponentClass<T>;
    /**
     * @brief Declare the Core metaclass hook for a BaseUnknown-derived object-model class.
     *
     * Place this macro inside every implementation, extension, and bound facet class. The macro obtains the enclosing
     * class as Self through P2996 reflection and exposes the Core_Old-style GetMetaStatic/GetMeta pair.
     */
    #define Y_OBJECT \
    public: \
        using Self = typename[: ::std::meta::access_context::current().scope():]; \
        static const ::Yuki::MetaClass& GetMetaStatic() noexcept { \
            static_assert(::std::derived_from<Self, ::Yuki::BaseUnknown>, \
                          "Y_OBJECT requires BaseUnknown inheritance"); \
            static_assert(::Yuki::DirectCppBaseCountOf<Self> <= 1, \
                          "Yuki object classes must use single inheritance"); \
            static_assert(::std::has_virtual_destructor_v<Self>, \
                          "Y_OBJECT requires a virtual destructor"); \
            return ::Yuki::StaticMetaClass<Self>(); \
        } \
        [[nodiscard]] const ::Yuki::MetaClass& GetMeta() const noexcept override { \
            return GetMetaStatic(); \
        } \
    public:

} // namespace Yuki
