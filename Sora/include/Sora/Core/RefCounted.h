/**
 * @file RefCounted.h
 * @brief Intrusive thread-safe reference counting, its CRTP base, and the owning pointer that drives it.
 * @ingroup Core
 *
 * @details @ref Sora::RefCounted adds an atomic intrusive reference count to a most-derived class without a control
 * block, without a virtual destructor, and without any allocation beyond the object itself. Ownership is expressed
 * through @ref Sora::IntrusivePtr; the raw pointer stays valid while at least one owning pointer refers to it.
 *
 * The count starts at one in the constructor. That constructor-provided reference belongs to the code that allocated
 * the object and must be transferred exactly once, through @ref Sora::MakeRefCounted, @ref Sora::AdoptRef, or
 * @ref Sora::IntrusivePtr::Adopt. Constructing a counted object on the stack, or deleting one directly, breaks the
 * protocol and is diagnosed by @ref Sora::Assert in debug builds.
 *
 * @par Relationship to @ref Sora::RefPtr
 * @ref Sora::RefPtr is Sora's non-owning observer. This header models the opposite role, so the owning smart pointer
 * is named @ref Sora::IntrusivePtr instead of overloading the established @c RefPtr name. It corresponds to Flutter's
 * @c fml::RefPtr, and @ref Sora::RefCounted corresponds to @c fml::RefCountedThreadSafe.
 *
 * @par Thread safety
 * Reference counting is atomic. Releasing the last reference publishes the object's destruction through an acquire
 * fence, so the destructor observes every write performed before the release. The stored pointer itself is not
 * atomic: a single owning pointer must not be used concurrently from several threads without synchronization.
 *
 * @code{.cpp}
 * class Texture final : public Sora::RefCounted<Texture> {
 * public:
 *     [[nodiscard]] static Sora::IntrusivePtr<Texture> Create(uint32_t width) {
 *         return Sora::MakeRefCounted<Texture>(width);
 *     }
 *
 * private:
 *     friend struct Sora::Detail::MakeRefCountedHelper<Texture>;
 *     friend class Sora::RefCounted<Texture>;
 *
 *     explicit Texture(uint32_t width) noexcept : width_(width) {}
 *     ~Texture() = default;
 *
 *     uint32_t width_ = 0;
 * };
 * @endcode
 *
 * @warning The template argument must name the most-derived type that owns the count. A counted base reached through
 * a non-virtual destructor deletes only the base subobject.
 */
#pragma once

#include <Sora/Core/Assertion.h>
#include <Sora/Platform.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Sora {

    template<typename T>
    class RefCounted;

    namespace Concept {

        /**
         * @brief Concept: @p T is a class that derives from its own @ref Sora::RefCounted specialization.
         *
         * @details The relation is checked against the cv-unqualified type, so @c const-qualified owning pointers
         * satisfy the same requirement as their mutable counterparts.
         */
        template<typename T>
        concept RefCountedObject
            = std::is_object_v<T> && std::derived_from<std::remove_cv_t<T>, RefCounted<std::remove_cv_t<T>>>;

    } // namespace Concept

    template<typename T>
    class IntrusivePtr;

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Debug-build adoption and destruction bookkeeping.
         *
         * @details Two flags are enough to tell a properly adopted object from one that escaped the protocol. The type
         * exists only as the debug half of @ref RefCountedState so that the same @ref RefCountedBase code compiles in
         * both build modes.
         */
        struct RefCountedDebugState {
            [[nodiscard]] constexpr bool AdoptionRequired() const noexcept { return adoptionRequired_; }
            [[nodiscard]] constexpr bool DestructionStarted() const noexcept { return destructionStarted_; }

            constexpr void MarkAdopted() noexcept { adoptionRequired_ = false; }
            constexpr void MarkDestroying() noexcept { destructionStarted_ = true; }

        private:
            bool adoptionRequired_ = true;
            bool destructionStarted_ = false;
        };

        /** @brief Release-build substitute that stores nothing and answers with constants. */
        struct RefCountedNoDebugState {
            [[nodiscard]] constexpr bool AdoptionRequired() const noexcept { return false; }
            [[nodiscard]] constexpr bool DestructionStarted() const noexcept { return false; }

            constexpr void MarkAdopted() noexcept {}
            constexpr void MarkDestroying() noexcept {}
        };

        /** @brief Bookkeeping storage selected by the build mode instead of by preprocessor branches. */
        using RefCountedState = std::conditional_t<Platform::kIsDebug, RefCountedDebugState, RefCountedNoDebugState>;

        /**
         * @brief Untyped atomic strong count shared by every @ref Sora::RefCounted specialization.
         *
         * @details The count starts at one. Adopting marks that constructor-provided reference as owned; the marker is
         * stored only in debug builds so that an unadopted object is caught before it is destroyed through the count.
         */
        class RefCountedBase {
        public:
            RefCountedBase(const RefCountedBase&) = delete;
            RefCountedBase& operator=(const RefCountedBase&) = delete;

            /** @brief Add one strong reference. @pre The object has been adopted and is not being destroyed. */
            void AddRef() const noexcept {
                if constexpr (Platform::kIsDebug) {
                    Assert(!debug_.AdoptionRequired(), "AddRef on a counted object that was never adopted");
                    Assert(!debug_.DestructionStarted(), "AddRef while the counted object is being destroyed");
                }
                refCount_.fetch_add(1, std::memory_order_relaxed);
            }

            /**
             * @brief Return whether exactly one strong reference remains.
             * @note The result is a snapshot. It is meaningful only while the caller itself owns that reference.
             */
            [[nodiscard]] bool HasOneRef() const noexcept { return refCount_.load(std::memory_order_acquire) == 1; }

            /** @brief Report in debug builds unless exactly one strong reference remains. */
            void AssertHasOneRef() const noexcept {
                if constexpr (Platform::kIsDebug) {
                    Assert(HasOneRef(), "expected the counted object to have exactly one strong reference");
                }
            }

        protected:
            RefCountedBase() noexcept = default;

            ~RefCountedBase() noexcept {
                if constexpr (Platform::kIsDebug) {
                    Assert(!debug_.AdoptionRequired(), "destroyed a counted object that was never adopted");
                    Assert(debug_.DestructionStarted(), "destroyed a counted object outside Release");
                }
            }

            /**
             * @brief Drop one strong reference.
             * @return @c true when this was the last reference and the caller must destroy the object.
             */
            [[nodiscard]] bool Release() const noexcept {
                if constexpr (Platform::kIsDebug) {
                    Assert(!debug_.AdoptionRequired(), "Release on a counted object that was never adopted");
                    Assert(!debug_.DestructionStarted(), "Release while the counted object is being destroyed");
                }
                if (refCount_.fetch_sub(1, std::memory_order_release) == 1) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    debug_.MarkDestroying();
                    return true;
                }
                return false;
            }

            /** @brief Mark the constructor-provided reference as owned. */
            void Adopt() const noexcept {
                if constexpr (Platform::kIsDebug) {
                    Assert(debug_.AdoptionRequired(), "the constructor-provided reference was already adopted");
                }
                debug_.MarkAdopted();
            }

        private:
            mutable std::atomic<std::uint32_t> refCount_{1};
            SORA_NO_UNIQUE_ADDRESS mutable RefCountedState debug_{};
        };

        static_assert(Platform::kIsDebug || sizeof(RefCountedBase) == sizeof(std::atomic<std::uint32_t>),
                      "debug-only reference-counting bookkeeping must not consume storage in release builds.");

        static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                      "Sora::RefCounted requires a lock-free 32-bit atomic so that reference counting never blocks.");

        /** @brief Friendship anchor that lets private constructors participate in @ref Sora::MakeRefCounted. */
        template<typename T>
        struct MakeRefCountedHelper;

    } // namespace Detail
    /** @endcond */

    /**
     * @brief CRTP base that adds an intrusive thread-safe reference count to @p T.
     *
     * @details Derive publicly from @c RefCounted<T> where @c T is the most-derived class. The count is part of the
     * object: there is no separate control block, no extra indirection, and no virtual dispatch. @ref IntrusivePtr is
     * the intended owner; @ref RefCounted::Release may also be called directly when the caller holds the initial
     * reference.
     *
     * Classes with private constructors or destructors friend @ref RefCounted and, when
     * @ref Sora::MakeRefCounted is used, @c Sora::Detail::MakeRefCountedHelper:
     *
     * @code{.cpp}
     * class Private final : public Sora::RefCounted<Private> {
     * private:
     *     friend struct Sora::Detail::MakeRefCountedHelper<Private>;
     *     friend class Sora::RefCounted<Private>;
     *     Private();
     *     ~Private();
     * };
     * @endcode
     *
     * @tparam T Most-derived object type. It must derive publicly from @c RefCounted<T>; the requirement is checked
     * whenever @ref IntrusivePtr<@p T> is instantiated.
     */
    template<typename T>
    class RefCounted : public Detail::RefCountedBase {
    public:
        /** @brief Add one strong reference. */
        using Detail::RefCountedBase::AddRef;

        /** @brief Debug-build assertion that exactly one strong reference remains. */
        using Detail::RefCountedBase::AssertHasOneRef;

        /** @brief Return whether exactly one strong reference remains. */
        using Detail::RefCountedBase::HasOneRef;

        RefCounted(const RefCounted&) = delete;
        RefCounted& operator=(const RefCounted&) = delete;

        /** @brief Drop one strong reference and destroy @p T when the count reaches zero. */
        void Release() const noexcept {
            if (Detail::RefCountedBase::Release()) {
                delete static_cast<const T*>(this);
            }
        }

    protected:
        /** @brief Construct with a single strong reference owned by the eventual adopter. */
        RefCounted() noexcept = default;

        /** @brief Destroy only through @ref Release or an @ref IntrusivePtr. */
        ~RefCounted() = default;

    private:
        template<typename U>
        friend class IntrusivePtr;

        /** @brief Mark the constructor-provided reference as owned by an @ref IntrusivePtr. */
        void Adopt() const noexcept { Detail::RefCountedBase::Adopt(); }
    };

    /**
     * @brief Owning intrusive pointer for @ref Sora::RefCounted objects.
     *
     * @details Copying retains, moving and release drop one reference, and the last release destroys the pointee
     * through its most-derived type. Both operations are @c noexcept and allocation-free. The stored pointer is the
     * only state, so an owning pointer costs exactly one pointer.
     *
     * @tparam T Most-derived counted type, optionally cv-qualified. @c IntrusivePtr<const T> retains and releases the
     * same count as @c IntrusivePtr<T> and permits read-only access.
     *
     * @warning Constructing from a raw pointer retains a borrowed reference; it does not take ownership of the
     * constructor-provided reference. Use @ref IntrusivePtr::Adopt for the latter.
     */
    template<typename T>
    class [[nodiscard]] IntrusivePtr {
        static_assert(std::is_object_v<T>, "Sora::IntrusivePtr<T> requires T to be an object type.");
        static_assert(Concept::RefCountedObject<T>,
                      "Sora::IntrusivePtr<T> requires T to derive publicly from Sora::RefCounted<T>.");

    public:
        /** @brief Observed element type. */
        using ElementType = T;

        /** @brief Stored raw pointer type, @c T*. */
        using Pointer = T*;

        /** @name Constructors @{ ------------------------------------------------------ */

        /** @brief Construct an empty pointer. */
        IntrusivePtr() noexcept = default;

        /** @brief Construct an empty pointer from @c nullptr. */
        IntrusivePtr(std::nullptr_t) noexcept {}

        /**
         * @brief Retain @p pointer and observe it.
         * @param[in] pointer Borrowed pointer that must already be owned by someone else.
         */
        explicit IntrusivePtr(Pointer pointer) noexcept : pointer_(pointer) { Retain(pointer_); }

        /** @brief Copy an owning pointer, adding one reference. */
        IntrusivePtr(const IntrusivePtr& other) noexcept : pointer_(other.pointer_) { Retain(pointer_); }

        /**
         * @brief Convert from a statically compatible owning pointer, adding one reference.
         * @tparam U Source element type whose @c U* converts to @c T*, typically the cv-qualified source.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        IntrusivePtr(const IntrusivePtr<U>& other) noexcept : pointer_(other.Get()) {
            Retain(pointer_);
        }

        /** @brief Move an owning pointer without touching the count. */
        IntrusivePtr(IntrusivePtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        /**
         * @brief Convert from a statically compatible owning pointer, transferring its reference.
         * @tparam U Source element type whose @c U* converts to @c T*, typically the cv-qualified source.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        IntrusivePtr(IntrusivePtr<U>&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

        /** @brief Take ownership of the constructor-provided reference in @p pointer. */
        [[nodiscard]] static IntrusivePtr Adopt(Pointer pointer) noexcept {
            if constexpr (Platform::kIsDebug) {
                if (pointer != nullptr) {
                    pointer->Adopt();
                }
            }
            return IntrusivePtr{pointer, AdoptTag{}};
        }

        /** @} ------------------------------------------------------------------------ */

        /** @name Assignment @{ ------------------------------------------------------- */

        /** @brief Rebind from another owning pointer, retaining the new pointee first. */
        IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
            if (this != &other) {
                Reset(other.pointer_);
            }
            return *this;
        }

        /** @brief Rebind from a statically compatible owning pointer, retaining the new pointee first. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        IntrusivePtr& operator=(const IntrusivePtr<U>& other) noexcept {
            Reset(other.Get());
            return *this;
        }

        /** @brief Take over @p other's reference and leave @p other empty. */
        IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
            if (this != &other) {
                Reset();
                pointer_ = std::exchange(other.pointer_, nullptr);
            }
            return *this;
        }

        /** @brief Take over @p other's reference and leave @p other empty. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        IntrusivePtr& operator=(IntrusivePtr<U>&& other) noexcept {
            Reset(other.Get());
            other.Reset();
            return *this;
        }

        /** @brief Retain @p pointer and release the currently owned reference. */
        IntrusivePtr& operator=(Pointer pointer) noexcept {
            Reset(pointer);
            return *this;
        }

        /** @brief Release the owned reference and become empty. */
        IntrusivePtr& operator=(std::nullptr_t) noexcept {
            Reset();
            return *this;
        }

        /** @} ------------------------------------------------------------------------ */

        /** @brief Release the owned reference, destroying the pointee when it was the last one. */
        ~IntrusivePtr() noexcept { Drop(pointer_); }

        /** @name Observers @{ -------------------------------------------------------- */

        /** @brief Return the raw pointer. The result may be null. */
        [[nodiscard]] Pointer Get() const noexcept { return pointer_; }

        /** @brief Return @c true iff this pointer owns a reference. */
        [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

        /** @brief Explicit conversion to the raw pointer. */
        [[nodiscard]] explicit operator Pointer() const noexcept { return pointer_; }

        /** @brief Dereference the pointee. @pre This pointer is non-null. */
        [[nodiscard]] T& operator*() const noexcept { return *pointer_; }

        /** @brief Access the pointee. @pre This pointer is non-null. */
        [[nodiscard]] Pointer operator->() const noexcept { return pointer_; }

        /** @} ------------------------------------------------------------------------ */

        /** @name Mutation @{ --------------------------------------------------------- */

        /**
         * @brief Retain @p pointer, then release the currently owned reference.
         * @param[in] pointer Replacement pointer, or @c nullptr. Passing the current pointer keeps the count intact.
         */
        void Reset(Pointer pointer = nullptr) noexcept {
            Pointer replacement = pointer;
            Retain(replacement);
            Drop(std::exchange(pointer_, replacement));
        }

        /** @brief Swap owned references with @p other without touching the count. */
        void Swap(IntrusivePtr& other) noexcept { std::swap(pointer_, other.pointer_); }

        /** @} ------------------------------------------------------------------------ */

        /** @name Comparison @{ ------------------------------------------------------- */

        /** @brief Compare two owning pointers by address. */
        [[nodiscard]] friend bool operator==(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
            return lhs.pointer_ == rhs.pointer_;
        }

        /** @brief Compare an owning pointer with null. */
        [[nodiscard]] friend bool operator==(const IntrusivePtr& pointer, std::nullptr_t) noexcept {
            return pointer.pointer_ == nullptr;
        }

        /** @} ------------------------------------------------------------------------ */

    private:
        template<typename U>
        friend class IntrusivePtr;

        struct AdoptTag {};

        IntrusivePtr(Pointer pointer, AdoptTag) noexcept : pointer_(pointer) {}

        static void Retain(Pointer pointer) noexcept {
            if (pointer != nullptr) {
                pointer->AddRef();
            }
        }

        static void Drop(Pointer pointer) noexcept {
            if (pointer != nullptr) {
                pointer->Release();
            }
        }

        Pointer pointer_{};
    };

    /**
     * @brief Adopt @p pointer's constructor-provided reference into an owning pointer.
     * @param[in] pointer Result of @c new, or @c nullptr.
     * @return Owning pointer that holds the reference the constructor created.
     * @pre @p pointer has never been adopted and its count is still one.
     */
    template<Concept::RefCountedObject T>
    [[nodiscard]] IntrusivePtr<T> AdoptRef(T* pointer) noexcept {
        return IntrusivePtr<T>::Adopt(pointer);
    }

    /** @cond INTERNAL */
    namespace Detail {

        template<typename T>
        struct MakeRefCountedHelper {
            template<typename... Args>
            [[nodiscard]] static IntrusivePtr<T> Create(Args&&... args) {
                return IntrusivePtr<T>::Adopt(new T(std::forward<Args>(args)...));
            }
        };

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Allocate @p T with @p args and return the owning pointer that holds its initial reference.
     * @tparam T Most-derived counted type to construct.
     * @tparam Args Constructor argument types.
     * @param[in] args Arguments forwarded to the constructor of @p T.
     * @return Owning pointer owning the freshly constructed object.
     * @note If construction throws, nothing was allocated for the count and no reference is leaked.
     * @note Private constructors are reachable when the class friends
     * @c Sora::Detail::MakeRefCountedHelper.
     */
    template<Concept::RefCountedObject T, typename... Args>
        requires(!std::is_const_v<T>)
    [[nodiscard]] IntrusivePtr<T> MakeRefCounted(Args&&... args) {
        return Detail::MakeRefCountedHelper<T>::Create(std::forward<Args>(args)...);
    }

    /** @brief Swap two owning pointers. */
    template<Concept::RefCountedObject T>
    void Swap(IntrusivePtr<T>& lhs, IntrusivePtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

    /** @brief Standard lower-case ADL swap for @ref IntrusivePtr. */
    template<Concept::RefCountedObject T>
    void swap(IntrusivePtr<T>& lhs, IntrusivePtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

} // namespace Sora
