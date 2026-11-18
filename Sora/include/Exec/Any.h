#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <meta>
#include <typeindex>

namespace Sora {

    struct TaggedPtr {
        static constexpr uintptr_t kTagMask = 1;

        constexpr TaggedPtr() noexcept : _data(kTagMask) {}
        explicit TaggedPtr(void* iPtr, bool iTag = true) noexcept : _data(std::bit_cast<uintptr_t>(iPtr) | static_cast<uintptr_t>(iTag)) {}

        [[nodiscard]] void* Get() const noexcept { return std::bit_cast<void*>(_data & ~kTagMask); }

        [[nodiscard]] constexpr bool IsTagged() const noexcept { return static_cast<bool>(_data & kTagMask); }

        [[nodiscard]] constexpr bool operator==(std::nullptr_t) const noexcept { return _data <= kTagMask; }

    private:
        uintptr_t _data;
    };

    enum class BoxType : uint8_t {
        Abstract,
        Object,
        Proxy,
    };

    enum class RootType : uint8_t {
        Value,
        Reference,
    };

    namespace Concept {

        namespace Detail {

            template<template<typename> typename Interface, typename Base>
            constexpr Interface<Base>& InterfaceCast(Interface<Base>& iArg) noexcept {
                return iArg;
            };

            template<template<typename> typename Interface, typename Base>
            constexpr Interface<Base> const& InterfaceCast(Interface<Base> const& iArg) noexcept {
                return iArg; // NOLINT(bugprone-return-const-ref-from-parameter)
            };

        } // namespace Detail

        template<typename Interfacce, BoxType Type>
        concept HasBoxKind = std::remove_reference_t<Interfacce>::kBoxKind == Type;

        template<typename Interface, template<typename> typename BaseInterface>
        concept ExtensionOf = requires(Interface const& iArg) { Detail::InterfaceCast<BaseInterface>(iArg); };

        template<typename Derived, template<typename> typename Interface>
        concept AlreadyImplements = requires(Derived const& iArg) { true; }; // todo:

    } // namespace Concept

    template<typename T>
    struct Box {
        constexpr explicit Box(T iValue) noexcept : _value(iValue) {}
        template<typename... Ts>
        constexpr explicit Box(Ts&&... iArgs) noexcept(std::is_nothrow_constructible_v<T, Ts...>)
            : _value{std::move(iArgs)...} {}
        template<typename Func, typename... Ts>
        constexpr explicit Box(std::in_place_t, Func&& iFunc, Ts&&... iArgs)
            noexcept(std::is_nothrow_constructible_v<T, Ts...>)
            : _value{std::invoke(std::forward<Func>(iFunc), std::move(iArgs)...)} {
            static_assert(std::same_as<std::invoke_result_t<Func, Ts...>, T>);
        }

        [[no_unique_address]] T _value;
    };

    template<template<typename> typename Interface, typename ExtendsForBaseInterfaces>
    using AbstractModel = Interface<void>; // todo:

    template<template<typename> typename Interface,
             typename Base,                     // the base interface, or iroot
             typename ExtendsForBaseInterfaces, //
             size_t BufferSize,                 // size of the core, excluding the vtable
             size_t BufferAlignment>
    struct InterfaceBase : Base // an intermediate class bridging Interfacec and its bases
    {
        using BaseInterfaces = ExtendsForBaseInterfaces;
        using ThisInterfaceType = AbstractModel<Interface, ExtendsForBaseInterfaces>;
        using Base::_IndirectBind_;
        using Base::_SliceTo_;

        static constexpr size_t kBufferSize = std::max(BufferSize + sizeof(void*), Base::kBufferSize);
        static constexpr size_t kBufferAlignment = std::max(BufferAlignment, Base::kBufferAlignment);
    };

    struct AbstractRoot {
        static constexpr BoxType kBoxType = BoxType::Abstract;

        static constexpr size_t kBufferSize = sizeof(TaggedPtr);
        static constexpr size_t kBufferAlignment = alignof(TaggedPtr);

        static constexpr std::vector<std::meta::info> kBaseTypes = {};

        constexpr virtual ~AbstractRoot() = default;
        constexpr virtual void _Delete_() noexcept { this->~AbstractRoot(); }

        [[nodiscard]] constexpr virtual bool _Empty_() const noexcept { std::unreachable(); }

        constexpr virtual void _Reset_() noexcept { std::unreachable(); }

        [[nodiscard]] constexpr virtual std::type_index const& _Type_() const noexcept { std::unreachable(); }

        [[nodiscard]] constexpr virtual void* _Data_() const noexcept { std::unreachable(); }

        void _SliceTo_() noexcept = delete;
        void _IndirectBind_() const noexcept = delete;
    };

    template<template<typename> typename Interface, typename Bases>
    using AbstractModel = [:[] consteval {

    }:];

    template<template<typename> typename Interface, typename Value>
    struct [[empty_bases]] ValueRoot : Box<Value> {};

} // namespace Sora
