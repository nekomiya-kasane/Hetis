#pragma once

#include "Sora/Kernel/Core/Traits.h"

#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace Sora::Kernel {

    /**
     * @brief Intrusive owning pointer for BaseUnknown-anchored concrete objects and interface facets.
     *
     * @details The stored pointer is the typed facet address. The retained reference is always held on the closure
     * nucleus returned by @ref BaseUnknown::Nucleus, so pointers to implementation objects, extensions, and bound TIE
     * facets have one uniform lifetime model.
     */
    template<Concept::ComClass T>
    class ComPtr {
    public:
        using ElementType = T;
        using Pointer = T*;

        /** @brief Construct an empty pointer. */
        constexpr ComPtr() noexcept = default;

        /** @brief Construct an empty pointer from null. */
        constexpr ComPtr(std::nullptr_t) noexcept {}

        /** @brief Construct from a borrowed BaseUnknown-anchored pointer and retain its nucleus. */
        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        explicit ComPtr(U* pointer) noexcept : pointer_(pointer) {
            Retain(NucleusOf(pointer_));
        }

        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        ComPtr(const ComPtr<U>& other) noexcept : pointer_(other.Get()) {
            Retain(NucleusOf(pointer_));
        }

        ComPtr(const ComPtr& other) noexcept : pointer_(other.pointer_) {
            Retain(NucleusOf(pointer_));
        }

        ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        ComPtr(ComPtr<U>&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        ComPtr& operator=(const ComPtr& other) noexcept {
            if (this != std::addressof(other)) {
                Reset(other.pointer_);
            }
            return *this;
        }

        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        ComPtr& operator=(const ComPtr<U>& other) noexcept {
            Reset(other.Get());
            return *this;
        }

        ComPtr& operator=(ComPtr&& other) noexcept {
            if (this != std::addressof(other)) {
                Release(NucleusOf(pointer_));
                pointer_ = std::exchange(other.pointer_, nullptr);
            }
            return *this;
        }

        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        ComPtr& operator=(ComPtr<U>&& other) noexcept {
            Release(NucleusOf(pointer_));
            pointer_ = std::exchange(other.pointer_, nullptr);
            return *this;
        }

        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        ComPtr& operator=(U* pointer) noexcept {
            Reset(pointer);
            return *this;
        }

        /** @brief Release the owned strong reference. */
        ~ComPtr() noexcept { Release(NucleusOf(pointer_)); }

        /** @brief Replace with a borrowed BaseUnknown-anchored pointer and retain the replacement. */
        template<Concept::ComClass U>
            requires std::convertible_to<U*, T*>
        void Reset(U* pointer) noexcept {
            T* replacement = pointer;
            Retain(NucleusOf(replacement));
            Release(NucleusOf(pointer_));
            pointer_ = replacement;
        }

        /** @brief Reset to null. */
        void Reset(std::nullptr_t = nullptr) noexcept {
            Release(NucleusOf(pointer_));
            pointer_ = nullptr;
        }

        enum class CompareResult : uint8_t { ExactlySame, SameClosure, DifferentClosure };

        [[nodiscard]] CompareResult Compare(const ComPtr& other) const noexcept {
            if (pointer_ == other.pointer_) {
                return CompareResult::ExactlySame;
            }
            if (pointer_ == nullptr || other.pointer_ == nullptr) {
                return CompareResult::DifferentClosure;
            }
            if (pointer_->Nucleus() == other.pointer_->Nucleus()) {
                return CompareResult::SameClosure;
            }
            return CompareResult::DifferentClosure;
        }

        /** @brief Return the typed raw pointer. */
        [[nodiscard]] T* Get() const noexcept { return pointer_; }

        /** @brief Dereference the typed pointer. */
        [[nodiscard]] T& operator*() const noexcept { return *pointer_; }

        /** @brief Access the typed pointer. */
        [[nodiscard]] T* operator->() const noexcept { return pointer_; }

        /** @brief Return whether this handle is non-null. */
        [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

        /** @brief Swap two owning handles without changing reference counts. */
        void Swap(ComPtr& other) noexcept {
            using std::swap;
            swap(pointer_, other.pointer_);
        }

    private:
        template<Concept::ComClass U>
        friend class ComPtr;

        struct AdoptTag {};

        explicit ComPtr(T* pointer, AdoptTag) noexcept : pointer_(pointer) {}

        [[nodiscard]] static BaseUnknown* NucleusOf(T* pointer) noexcept {
            return pointer ? pointer->Nucleus() : nullptr;
        }

        template<Concept::ComClass U, class... Args>
        friend ComPtr<U> MakeComPtr(Args&&... args);

        Pointer pointer_{};
    };

    /** @brief Allocate @p T and adopt its initial intrusive storage reference. */
    template<Concept::ComClass T, class... Args>
    [[nodiscard]] ComPtr<T> MakeComPtr(Args&&... args) {
        return ComPtr<T>{new T(std::forward<Args>(args)...), typename ComPtr<T>::AdoptTag{}};
    }

    /** @brief Swap two owning component pointers. */
    template<Concept::ComClass T>
    void Swap(ComPtr<T>& lhs, ComPtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

} // namespace Sora::Kernel
