/**
 * @file ComAdaptor.h
 * @brief Generic COM nucleus adaptor for ordinary C++ object values.
 * @ingroup Core
 */
#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ComPtr.h"

#include <concepts>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Sora::Kernel {

    /**
     * @brief External declaration of how @ref ComAdaptor should expose wrapped type @p T to the object model.
     *
     * @details Specialise this template outside @p T when a plain C++ type should be wrapped as a Sora COM class.
     * The default intentionally declares no object-model role, so wrapping a value never silently turns it into an
     * implementation class.
     *
     * @code{.cpp}
     * template<>
     * struct Sora::Kernel::ComAdaptorDecl<MyExtensionState> {
     *     static constexpr TypeOfClass role = TypeOfClass::DataExtension;
     *     using Extends = Sora::Traits::TypeList<MyComponent>;
     *     using Implements = Sora::Traits::TypeList<IMyFacet>;
     * };
     * @endcode
     */
    template<typename T>
    struct ComAdaptorDecl {
        static constexpr TypeOfClass role = TypeOfClass::NothingType;
        using Extends = Sora::Traits::TypeList<>;
        using Implements = Sora::Traits::TypeList<>;
    };

    namespace Detail {

        template<typename T>
        inline constexpr TypeOfClass ComAdaptorRoleOf = ComAdaptorDecl<T>::role;

        template<typename T>
        using ComAdaptorImplements =
            Sora::Traits::ApplyT<Sora::Kernel::$::Implements, typename ComAdaptorDecl<T>::Implements>;

        template<typename T>
        using ComAdaptorExtends =
            Sora::Traits::ApplyT<Sora::Kernel::$::Extends, typename ComAdaptorDecl<T>::Extends>;

    } // namespace Detail

    template<typename T>
        requires std::is_object_v<T>
    class ComAdaptor;

    namespace Traits {

        template<typename T>
        inline constexpr TypeOfClass RoleOf<ComAdaptor<T>> = Detail::ComAdaptorRoleOf<T>;

    } // namespace Traits

    /**
     * @brief Wrap an ordinary object value in a @ref BaseUnknown object-model nucleus.
     *
     * @details The adaptor gives non-COM state a first-class place in the Sora object model. The wrapped object is the
     * object-model payload, while the adaptor itself owns the intrusive lifetime and closure graph. Its role and
     * object-model edges are declared by specialising @ref ComAdaptorDecl for @p T.
     *
     * @tparam T Wrapped object type stored directly inside the adaptor.
     */
    template<typename T>
        requires std::is_object_v<T>
    class [[= Sora::Kernel::$::Role{ComAdaptorDecl<T>::role}, = Detail::ComAdaptorExtends<T>{},
            = Detail::ComAdaptorImplements<T>{}]] ComAdaptor : public BaseUnknown {
    public:
        using Self = ComAdaptor<T>;
        using ValueType = T;

        using Extends = typename ComAdaptorDecl<T>::Extends;
        using Implements = typename ComAdaptorDecl<T>::Implements;

        /** @brief Construct the wrapped object with its default constructor. */
        constexpr ComAdaptor() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
            : object_{} {}

        /** @brief Construct the wrapped object directly from @p args. */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        explicit constexpr ComAdaptor(std::in_place_t,
                                      Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
            : object_(std::forward<Args>(args)...) {}

        /** @brief Move a complete object value into the adaptor. */
        explicit constexpr ComAdaptor(T object) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
            : object_(std::move(object)) {}

        [[nodiscard]] static Iid GetIidStatic() noexcept { return Traits::IidOf<Self>; }
        [[nodiscard]] Iid GetIid() const noexcept override { return Self::GetIidStatic(); }

        [[nodiscard]] static std::shared_ptr<const MetaClass> GetMetaStatic() noexcept {
            return MetaClass::Query<Self>();
        }

        [[nodiscard]] std::shared_ptr<const MetaClass> GetMeta() const noexcept override {
            return Self::GetMetaStatic();
        }

        [[nodiscard]] static std::string_view GetClassNameStatic() noexcept {
            return Self::GetMetaStatic()->GetClassName();
        }

        [[nodiscard]] std::string_view GetClassName() const noexcept override { return Self::GetClassNameStatic(); }

        [[nodiscard]] static TypeOfClass GetRoleStatic() noexcept { return Detail::ComAdaptorRoleOf<T>; }
        [[nodiscard]] TypeOfClass GetRole() const noexcept override { return Self::GetRoleStatic(); }

        /** @brief Return the mutable wrapped object. */
        [[nodiscard]] constexpr T& Object() & noexcept { return object_; }

        /** @brief Return the const wrapped object. */
        [[nodiscard]] constexpr const T& Object() const& noexcept { return object_; }

        /** @brief Move out the wrapped object from an rvalue adaptor. */
        [[nodiscard]] constexpr T&& Object() && noexcept { return std::move(object_); }

        /** @brief Alias for @ref Object. */
        [[nodiscard]] constexpr T& Get() & noexcept { return object_; }

        /** @brief Alias for @ref Object. */
        [[nodiscard]] constexpr const T& Get() const& noexcept { return object_; }

        /** @brief Pointer-like access to the wrapped object. */
        [[nodiscard]] constexpr T* operator->() noexcept { return std::addressof(object_); }

        /** @brief Pointer-like access to the wrapped object. */
        [[nodiscard]] constexpr const T* operator->() const noexcept { return std::addressof(object_); }

        /** @brief Dereference to the wrapped object. */
        [[nodiscard]] constexpr T& operator*() & noexcept { return object_; }

        /** @brief Dereference to the wrapped object. */
        [[nodiscard]] constexpr const T& operator*() const& noexcept { return object_; }

        /** @brief Dereference to the wrapped object and preserve rvalue category. */
        [[nodiscard]] constexpr T&& operator*() && noexcept { return std::move(object_); }

        /** @brief Dereference to the wrapped object and preserve const rvalue category. */
        [[nodiscard]] constexpr const T&& operator*() const&& noexcept { return std::move(object_); }

    private:
        T object_;
    };

    /** @brief Allocate a @ref ComAdaptor wrapping @p T and return an owning COM pointer. */
    template<typename T, typename... Args>
        requires std::is_object_v<T> && std::constructible_from<T, Args...>
    [[nodiscard]] ComPtr<ComAdaptor<T>> MakeComAdaptor(Args&&... args) {
        return MakeComPtr<ComAdaptor<T>>(std::in_place, std::forward<Args>(args)...);
    }

} // namespace Sora::Kernel
