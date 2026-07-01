/**
 * @file ComPtr.h
 * @brief Intrusive owning handle for BaseUnknown objects and interface facets.
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "Yuki/Core/BaseUnknown.h"
#include "Yuki/Core/Meta.h"

namespace Yuki {

    namespace Detail {

        /** @brief Runtime-checked QueryInterface kernel used when static pointer compatibility is not provable. */
        [[nodiscard]] BaseUnknown* QueryInterfaceFacet(BaseUnknown* object, Iid iid,
                                                       BaseUnknown*& retainedAnchor) noexcept;

        /** @brief Private constructor access for query code that already owns a retained anchor. */
        struct ComPtrAccess;

    } // namespace Detail

    /**
     * @brief Intrusive owning pointer for BaseUnknown-anchored concrete objects and interface facets.
     *
     * @details @ref ComPtr is intentionally constrained at the class-template boundary: every element type must be
     * @ref BaseUnknown itself or inherit from it. This keeps the handle's ownership model single-valued: the stored
     * typed pointer is only an address, while the retained reference always belongs to @ref Anchor().
     */
    template<class T>
        requires Detail::BaseUnknownRawClass<T>
    class ComPtr {
    public:
        /** @brief Construct an empty pointer. */
        constexpr ComPtr() noexcept = default;

        /** @brief Construct an empty pointer from null. */
        constexpr ComPtr(std::nullptr_t) noexcept {}

        /** @brief Construct from a borrowed BaseUnknown-anchored pointer and retain its nucleus. */
        explicit ComPtr(T* pointer) noexcept : pointer_(pointer), anchor_(pointer ? pointer->Nucleus() : nullptr) {
            Retain(anchor_);
        }

        /**
         * @brief Adopt one existing strong reference from @p pointer.
         *
         * @details When @c U* is statically convertible to @c T*, adoption is a pure compile-time checked pointer
         * adjustment. Otherwise Core performs a runtime metaclass / QueryInterface check. A failed runtime check
         * consumes and releases the input reference and returns an empty pointer.
         */
        template<class U>
            requires Detail::BaseUnknownRawClass<U>
        [[nodiscard]] static ComPtr Adopt(U* pointer) noexcept {
            ComPtr result;
            if (!pointer) {
                return result;
            }

            if constexpr (std::convertible_to<U*, T*>) {
                result.pointer_ = static_cast<T*>(pointer);
                result.anchor_ = pointer->Nucleus();
                return result;
            } else {
                return AdoptRuntime(pointer);
            }
        }

        /** @brief Copy and retain the referenced anchor. */
        ComPtr(const ComPtr& other) noexcept : pointer_(other.pointer_), anchor_(other.anchor_) { Retain(anchor_); }

        /** @brief Convert from a statically compatible ComPtr and retain the referenced anchor. */
        template<class U>
            requires Detail::BaseUnknownRawClass<U> && (!std::same_as<T, U>) && std::convertible_to<U*, T*>
        ComPtr(const ComPtr<U>& other) noexcept : pointer_(other.pointer_), anchor_(other.anchor_) {
            Retain(anchor_);
        }

        /** @brief Move by stealing the raw pointer and anchor. */
        ComPtr(ComPtr&& other) noexcept
            : pointer_(std::exchange(other.pointer_, nullptr)), anchor_(std::exchange(other.anchor_, nullptr)) {}

        /** @brief Move-convert from a statically compatible ComPtr by stealing its raw pointer and anchor. */
        template<class U>
            requires Detail::BaseUnknownRawClass<U> && (!std::same_as<T, U>) && std::convertible_to<U*, T*>
        ComPtr(ComPtr<U>&& other) noexcept
            : pointer_(static_cast<T*>(std::exchange(other.pointer_, nullptr))),
              anchor_(std::exchange(other.anchor_, nullptr)) {}

        /** @brief Release the owned strong reference. */
        ~ComPtr() noexcept { Release(anchor_); }

        /** @brief Copy-and-swap assignment. */
        ComPtr& operator=(ComPtr other) noexcept {
            Swap(other);
            return *this;
        }

        /** @brief Reset to null. */
        ComPtr& operator=(std::nullptr_t) noexcept {
            ComPtr{}.Swap(*this);
            return *this;
        }

        /** @brief Replace with a borrowed BaseUnknown-anchored pointer and retain the replacement. */
        void Reset(T* pointer = nullptr) noexcept {
            ComPtr replacement{pointer};
            Swap(replacement);
        }

        /** @brief Swap two pointer handles without touching reference counts. */
        void Swap(ComPtr& other) noexcept {
            std::swap(pointer_, other.pointer_);
            std::swap(anchor_, other.anchor_);
        }

        /** @brief Return the typed raw pointer. */
        [[nodiscard]] T* Get() const noexcept { return pointer_; }

        /** @brief Return the retained lifetime anchor. */
        [[nodiscard]] BaseUnknown* Anchor() const noexcept { return anchor_; }

        /** @brief Surrender ownership without releasing the strong reference. */
        [[nodiscard]] T* Detach() noexcept {
            anchor_ = nullptr;
            return std::exchange(pointer_, nullptr);
        }

        /** @brief Dereference the raw pointer. */
        T& operator*() const noexcept { return *pointer_; }

        /** @brief Access the raw pointer. */
        T* operator->() const noexcept { return pointer_; }

        /** @brief Return whether this handle is non-null. */
        explicit operator bool() const noexcept { return pointer_ != nullptr; }

    private:
        template<class U>
            requires Detail::BaseUnknownRawClass<U>
        friend class ComPtr;
        friend struct Detail::ComPtrAccess;

        /** @brief Build a handle from an already-retained query result without resolving it again. */
        [[nodiscard]] static ComPtr AdoptRetained(T* pointer, BaseUnknown* retainedAnchor) noexcept {
            ComPtr result;
            if (!pointer || !retainedAnchor) {
                Release(retainedAnchor);
                return result;
            }
            result.pointer_ = pointer;
            result.anchor_ = retainedAnchor;
            return result;
        }

        /** @brief Return @p object as @p Target after checking its metaclass inheritance chain. */
        template<class Target>
            requires Detail::BaseUnknownRawClass<Target>
        [[nodiscard]] static Target* RuntimeObjectCast(BaseUnknown* object) noexcept {
            if constexpr (Traits::YObjectClass<Target>) {
                for (const MetaClass* meta = &object->GetMeta(); meta; meta = meta->DirectBase()) {
                    if (meta->IidValue() == IidOf<Target>()) {
                        return static_cast<Target*>(object);
                    }
                }
            }
            return nullptr;
        }

        /** @brief Runtime compatibility path for adopting from a BaseUnknown-backed but non-convertible pointer. */
        template<class U>
            requires Detail::BaseUnknownRawClass<U>
        [[nodiscard]] static ComPtr AdoptRuntime(U* pointer) noexcept {
            ComPtr result;
            BaseUnknown* object = static_cast<BaseUnknown*>(pointer);
            if (T* typed = RuntimeObjectCast<T>(object)) {
                result.pointer_ = typed;
                result.anchor_ = object->Nucleus();
                return result;
            }

            BaseUnknown* retainedAnchor = nullptr;
            BaseUnknown* facet = Detail::QueryInterfaceFacet(object, IidOf<T>(), retainedAnchor);
            Release(object);
            return AdoptRetained(static_cast<T*>(facet), retainedAnchor);
        }

        T* pointer_{};
        BaseUnknown* anchor_{};
    };

    namespace Detail {

        /** @brief Internal factory for adopting query results that already hold their anchor reference. */
        struct ComPtrAccess {
            /** @brief Return a ComPtr that consumes @p retainedAnchor without retaining it again. */
            template<class T>
                requires BaseUnknownRawClass<T>
            [[nodiscard]] static ComPtr<T> AdoptRetained(T* pointer, BaseUnknown* retainedAnchor) noexcept {
                return ComPtr<T>::AdoptRetained(pointer, retainedAnchor);
            }
        };

    } // namespace Detail

    /** @brief Allocate @p T and return an adopted owning pointer. */
    template<class T, class... Args>
        requires Detail::BaseUnknownRawClass<T>
    [[nodiscard]] ComPtr<T> MakeOwned(Args&&... args) {
        static_assert(Traits::YObjectClass<T>,
                      "MakeOwned<T>() requires a directly annotated BaseUnknown object class.");
        static_assert(
            requires {
                { T::GetMetaStatic() } -> std::same_as<const MetaClass&>;
            }, "MakeOwned<T>() requires Y_OBJECT inside T.");
        auto* raw = new T(std::forward<Args>(args)...);
        return ComPtr<T>::Adopt(raw);
    }

} // namespace Yuki
