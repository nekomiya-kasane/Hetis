/**
 * @file UniqueResource.h
 * @brief Unique ownership for handles and resources released by an independent callable.
 * @ingroup Core
 *
 * @details @ref UniqueResource applies RAII to file descriptors, C handles, mapped addresses, and other resources that
 * do not encode destruction in their own type. It supports value and lvalue-reference resources, move-only resources
 * and deleters, checked invalid sentinels, and transactional construction.
 *
 * @code{.cpp}
 * FILE* file = std::fopen("scene.bin", "rb");
 * if (file == nullptr) {
 *     return;
 * }
 * auto closeFile = Sora::MakeUniqueResourceChecked(file, nullptr, [](FILE* value) noexcept {
 *     std::fclose(value);
 * });
 * @endcode
 *
 * A deleter is invoked from a non-throwing reset/destruction path and therefore must not allow an exception to escape.
 */
#pragma once

#include <Sora/Core/Traits/ConstructionTraits.h>
#include <Sora/Platform.h>

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @cond INTERNAL */
    namespace Detail {

        template<typename Resource, typename Deleter>
        concept ResourceOwner =
            !std::is_rvalue_reference_v<Resource> && std::is_object_v<std::remove_reference_t<Resource>> &&
            std::is_object_v<Deleter> && std::invocable<Deleter&, Resource&>;

        /** @brief Permanently store a value while rolling back a failed construction through a supplied operation. */
        template<typename T>
        class TransactionalValue {
        public:
            constexpr TransactionalValue() = default;
            constexpr TransactionalValue(const TransactionalValue&) = default;
            constexpr TransactionalValue& operator=(const TransactionalValue&) = default;
            constexpr TransactionalValue& operator=(TransactionalValue&&) = default;

            template<typename U>
                requires(!std::same_as<std::remove_cvref_t<U>, TransactionalValue> && std::constructible_from<T, U>)
            constexpr explicit TransactionalValue(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>)
                : value_{std::forward<U>(value)} {}

            template<typename U, typename Deleter, typename Resource>
                requires std::is_nothrow_constructible_v<T, U>
            constexpr TransactionalValue(U&& value, Deleter&, Resource&) noexcept : value_{std::forward<U>(value)} {}

            template<typename U, typename Deleter, typename Resource>
                requires(!std::is_nothrow_constructible_v<T, U> && std::constructible_from<T, U> &&
                         std::invocable<Deleter&, Resource&>)
            constexpr TransactionalValue(U&& value, Deleter& deleter, Resource& resource) try
                : value_{std::forward<U>(value)} {
            } catch (...) {
                std::invoke(deleter, resource);
                throw;
            }

            template<typename U, typename Rollback>
                requires(std::is_nothrow_constructible_v<T, U> && std::invocable<Rollback&>)
            constexpr TransactionalValue(U&& value, Rollback&&) noexcept : value_{std::forward<U>(value)} {}

            template<typename U, typename Rollback>
                requires(!std::is_nothrow_constructible_v<T, U> && std::constructible_from<T, U> &&
                         std::invocable<Rollback&>)
            constexpr TransactionalValue(U&& value, Rollback&& rollback) try : value_{std::forward<U>(value)} {
            } catch (...) {
                std::invoke(rollback);
                throw;
            }

            constexpr TransactionalValue(TransactionalValue&& other) noexcept(
                Concept::NothrowForwardConstructible<T, T>)
                requires Concept::SourcePreservingConstructible<T, T>
                : value_{ForwardForConstruction<T, T>(other.value_)} {}

            [[nodiscard]] constexpr T& Get() noexcept { return value_; }
            [[nodiscard]] constexpr const T& Get() const noexcept { return value_; }

        private:
            NO_UNIQUE_ADDRESS T value_{};
        };

        template<typename Resource>
        using StoredResource = std::conditional_t<std::is_lvalue_reference_v<Resource>,
                                                  std::reference_wrapper<std::remove_reference_t<Resource>>, Resource>;

        /** @brief Recover the declared resource reference from its value-or-reference-wrapper storage. */
        template<typename Resource, typename Stored>
        [[nodiscard]] constexpr decltype(auto) AccessResource(Stored& stored) noexcept {
            if constexpr (std::is_lvalue_reference_v<Resource>) {
                return stored.get();
            } else {
                return (stored);
            }
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Move-only owner for a resource and its independently supplied deleter.
     *
     * @details The resource can be an object or lvalue reference. The deleter is stored by value and invoked with an
     * lvalue reference to the resource. Construction and ownership transfer copy instead of using a potentially
     * throwing move where necessary to preserve cleanup responsibility.
     *
     * @tparam Resource Owned resource type; rvalue references and function types are not supported.
     * @tparam Deleter Object type invoked to release a @c Resource.
     */
    template<typename Resource, typename Deleter>
        requires Detail::ResourceOwner<Resource, Deleter>
    class [[nodiscard]] UniqueResource {
        using ResourceStorage = Detail::TransactionalValue<Detail::StoredResource<Resource>>;
        using DeleterStorage = Detail::TransactionalValue<Deleter>;

        struct DisarmedTag {};

        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) ResourceValue(this Self& self) noexcept {
            return Detail::AccessResource<Resource>(self.resource_.Get());
        }

        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) DeleterValue(this Self& self) noexcept {
            return self.deleter_.Get();
        }

    public:
        /** @brief Construct an empty owner when both stored objects are default constructible. */
        constexpr UniqueResource() = default;

        /**
         * @brief Store @p resource and @p deleter and assume ownership.
         * @details If either stored object construction fails, @p deleter releases the resource exactly once.
         */
        template<typename ResourceSource, typename DeleterSource>
            requires Concept::SourcePreservingConstructible<ResourceStorage, ResourceSource> &&
                         Concept::SourcePreservingConstructible<Deleter, DeleterSource>
        constexpr UniqueResource(ResourceSource&& resource, DeleterSource&& deleter) noexcept(
            Concept::NothrowForwardConstructible<ResourceStorage, ResourceSource> &&
            Concept::NothrowForwardConstructible<Deleter, DeleterSource>)
            : resource_{ForwardForConstruction<ResourceStorage, ResourceSource>(resource), deleter,
                        resource},
              deleter_{ForwardForConstruction<Deleter, DeleterSource>(deleter), deleter,
                       ResourceValue()},
              armed_{true} {}

        UniqueResource(const UniqueResource&) = delete;
        UniqueResource& operator=(const UniqueResource&) = delete;

        /** @brief Transfer ownership when both stored objects can be moved without throwing. */
        constexpr UniqueResource(UniqueResource&& other) noexcept
            requires(std::is_nothrow_move_constructible_v<ResourceStorage> &&
                     std::is_nothrow_move_constructible_v<Deleter>)
            : resource_{std::move(other.resource_)},
              deleter_{std::move(other.deleter_)},
              armed_{std::exchange(other.armed_, false)} {}

        /** @brief Transfer ownership with rollback when deleter construction can throw. */
        constexpr UniqueResource(UniqueResource&& other) noexcept(
            Concept::NothrowForwardConstructible<Deleter, Deleter>)
            requires(std::is_nothrow_move_constructible_v<ResourceStorage> &&
                     !std::is_nothrow_move_constructible_v<Deleter> &&
                     Concept::SourcePreservingConstructible<Deleter, Deleter>)
            : resource_{std::move(other.resource_)},
              deleter_{ForwardForConstruction<Deleter, Deleter>(other.DeleterValue()),
                       [&]() noexcept(noexcept(std::invoke(other.DeleterValue(), ResourceValue()))) {
                           if (other.armed_) {
                               std::invoke(other.DeleterValue(), ResourceValue());
                               other.Release();
                           }
                       }},
              armed_{std::exchange(other.armed_, false)} {}

        /** @brief Transfer ownership by copying both stored objects before changing @p other. */
        constexpr UniqueResource(UniqueResource&& other) noexcept(
            std::is_nothrow_constructible_v<ResourceStorage, Resource&> &&
            std::is_nothrow_constructible_v<DeleterStorage, Deleter&>)
            requires(!std::is_nothrow_move_constructible_v<ResourceStorage> &&
                     std::constructible_from<ResourceStorage, Resource&> &&
                     std::constructible_from<DeleterStorage, Deleter&>)
            : UniqueResource{other.ResourceValue(), other.DeleterValue(), DisarmedTag{}} {
            armed_ = std::exchange(other.armed_, false);
        }

        /** @brief Release the owned resource when still armed. */
        constexpr ~UniqueResource() noexcept { Reset(); }

        /** @brief Replace this owner by moving from @p other. */
        constexpr UniqueResource&
        operator=(UniqueResource&& other) noexcept(std::is_nothrow_move_assignable_v<ResourceStorage> &&
                                                   std::is_nothrow_move_assignable_v<DeleterStorage>)
            requires((std::is_nothrow_move_assignable_v<ResourceStorage> ||
                      std::is_assignable_v<ResourceStorage&, ResourceStorage&>) &&
                     (std::is_nothrow_move_assignable_v<DeleterStorage> ||
                      std::is_assignable_v<DeleterStorage&, DeleterStorage&>))
        {
            Reset();
            if constexpr (std::is_nothrow_move_assignable_v<ResourceStorage>) {
                if constexpr (std::is_nothrow_move_assignable_v<DeleterStorage>) {
                    resource_ = std::move(other.resource_);
                    deleter_ = std::move(other.deleter_);
                } else {
                    deleter_ = other.deleter_;
                    resource_ = std::move(other.resource_);
                }
            } else {
                resource_ = other.resource_;
                if constexpr (std::is_nothrow_move_assignable_v<DeleterStorage>) {
                    deleter_ = std::move(other.deleter_);
                } else {
                    deleter_ = other.deleter_;
                }
            }
            armed_ = std::exchange(other.armed_, false);
            return *this;
        }

        /** @brief Release the current resource exactly once and become empty. */
        constexpr void Reset() noexcept {
            if (armed_) {
                armed_ = false;
                std::invoke(DeleterValue(), ResourceValue());
            }
        }

        /**
         * @brief Release the current resource, replace it with @p resource, and assume ownership of the replacement.
         * @details If replacement assignment throws, this object remains disarmed.
         */
        template<typename ResourceSource>
            requires(std::is_nothrow_assignable_v<Resource&, ResourceSource> ||
                     std::is_assignable_v<Resource&, const std::remove_reference_t<ResourceSource>&>)
        constexpr void Reset(ResourceSource&& resource) {
            Reset();
            if constexpr (std::is_nothrow_assignable_v<Resource&, ResourceSource>) {
                ResourceValue() = std::forward<ResourceSource>(resource);
            } else {
                ResourceValue() = static_cast<const std::remove_reference_t<ResourceSource>&>(resource);
            }
            armed_ = true;
        }

        /** @brief Relinquish ownership without invoking the deleter. */
        constexpr void Release() noexcept { armed_ = false; }

        /** @brief Return the stored resource. */
        [[nodiscard]] constexpr const Resource& Get() const noexcept { return ResourceValue(); }

        /** @brief Dereference a non-void pointer resource. */
        [[nodiscard]] constexpr std::add_lvalue_reference_t<std::remove_pointer_t<Resource>> operator*() const noexcept
            requires(std::is_pointer_v<Resource> && !std::is_void_v<std::remove_pointer_t<Resource>>)
        {
            return *Get();
        }

        /** @brief Access a pointer resource. */
        [[nodiscard]] constexpr Resource operator->() const noexcept
            requires std::is_pointer_v<Resource>
        {
            return Get();
        }

        /** @brief Return the stored deleter. */
        [[nodiscard]] constexpr const Deleter& GetDeleter() const noexcept { return DeleterValue(); }

    private:
        template<typename ResourceSource, typename DeleterSource>
        constexpr UniqueResource(ResourceSource&& resource, DeleterSource&& deleter, DisarmedTag) noexcept(
            std::is_nothrow_constructible_v<ResourceStorage, ResourceSource> &&
            std::is_nothrow_constructible_v<DeleterStorage, DeleterSource>)
            : resource_{std::forward<ResourceSource>(resource)}, deleter_{std::forward<DeleterSource>(deleter)} {}

        template<typename ResourceSource, typename Sentinel, typename DeleterSource>
        friend constexpr UniqueResource<std::decay_t<ResourceSource>, std::decay_t<DeleterSource>>
        MakeUniqueResourceChecked(ResourceSource&&, const Sentinel&, DeleterSource&&) noexcept(
            noexcept(std::declval<ResourceSource&>() == std::declval<const Sentinel&>()) &&
            std::is_nothrow_constructible_v<std::decay_t<ResourceSource>, ResourceSource> &&
            std::is_nothrow_constructible_v<std::decay_t<DeleterSource>, DeleterSource>);

        NO_UNIQUE_ADDRESS ResourceStorage resource_{};
        NO_UNIQUE_ADDRESS DeleterStorage deleter_{};
        bool armed_ = false;
    };

    /** @brief Deduce resource and deleter storage types by value. */
    template<typename Resource, typename Deleter>
    UniqueResource(Resource, Deleter) -> UniqueResource<Resource, Deleter>;

    /**
     * @brief Create a unique resource that is disarmed when @p resource equals @p invalid.
     * @param[in] resource Candidate resource value.
     * @param[in] invalid Sentinel that denotes a resource requiring no cleanup.
     * @param[in] deleter Cleanup operation stored by the result.
     * @return Owner armed exactly when @p resource does not compare equal to @p invalid.
     */
    template<typename Resource, typename Sentinel, typename Deleter>
    [[nodiscard]] constexpr UniqueResource<std::decay_t<Resource>, std::decay_t<Deleter>> MakeUniqueResourceChecked(
        Resource&& resource, const Sentinel& invalid,
        Deleter&& deleter) noexcept(noexcept(std::declval<Resource&>() == std::declval<const Sentinel&>()) &&
                                    std::is_nothrow_constructible_v<std::decay_t<Resource>, Resource> &&
                                    std::is_nothrow_constructible_v<std::decay_t<Deleter>, Deleter>) {
        using Result = UniqueResource<std::decay_t<Resource>, std::decay_t<Deleter>>;
        if (static_cast<bool>(resource == invalid)) {
            return Result{std::forward<Resource>(resource), std::forward<Deleter>(deleter),
                          typename Result::DisarmedTag{}};
        }
        return Result{std::forward<Resource>(resource), std::forward<Deleter>(deleter)};
    }

} // namespace Sora
