/**
 * @file LayeredInterfaces.h
 * @brief Reflection-native construction of ordered, de-duplicated interface layers.
 * @ingroup Core
 *
 * @details
 * A layered interface is a unary class template applied to a base type. A
 * `LayeredInterfaceDescriptor` records that template and the ordered interface
 * dependencies that must be applied before it. The resulting inheritance
 * hierarchy is linearized in declaration order, matching stdexec's
 * `__extends` type algebra.
 *
 * One descriptor graph can construct three related representations. `Base::Layer`
 * identifies whether an interface is being formed as an abstract contract, an
 * owning object model, or a forwarding proxy model. Duplicate suppression is
 * scoped to that layer: an inherited abstract interface therefore does not
 * suppress the corresponding object or proxy override.
 *
 * @par Relation to stdexec
 * This facility models the algebra of stdexec's `__bases_of`,
 * `__already_implements`, and `__extends`. It deliberately does not prescribe
 * type-erased storage, allocation, or ownership. A domain supplies those policies
 * through its roots. As in stdexec, a typical interface template is written once;
 * applying it to roots from different layers creates the abstract, object, and
 * proxy virtual-function implementations. The abstract specialization provides
 * a virtual slot whose body reports misuse by terminating; it is an abstract
 * representation layer, not a C++ pure-virtual declaration.
 *
 * @par Reading the example
 * `CounterDescriptor` is a dependency DAG: `Incrementable` and
 * `Resettable` both require `Readable`. `AbstractCounter` linearizes that DAG into
 * one abstract virtual contract. `CounterObject` forwards the contract to an owned
 * `CounterModel`; `CounterProxy` forwards it through an `AbstractCounter&`.
 * Each capability and the facade contributes a call counter in the object and
 * proxy layers, showing that an interface may own representation-specific data
 * as well as virtual behavior.
 *
 * @par Complete example
 * @code{.cpp}
 * #include <Sora/Core/Polymorphism/LayeredInterfaces.h>
 *
 * #include <cassert>
 * #include <concepts>
 * #include <cstddef>
 * #include <exception>
 * #include <type_traits>
 * #include <utility>
 *
 * namespace CounterExample {
 *
 *     using Sora::InterfaceLayer;
 *
 *     struct InterfaceCallState {
 *         mutable std::size_t calls{};
 *     };
 *
 *     template<typename>
 *     struct EmptyInterfaceState {};
 *
 *     // Abstract interfaces carry no counter; object and proxy interfaces do.
 *     template<InterfaceLayer Layer, typename Tag>
 *     using InterfaceState
 *         = std::conditional_t<Layer == InterfaceLayer::Abstract, EmptyInterfaceState<Tag>, InterfaceCallState>;
 *
 *     struct AbstractRoot {
 *         static constexpr InterfaceLayer Layer = InterfaceLayer::Abstract;
 *         virtual ~AbstractRoot() = default;
 *     };
 *
 *     // One template defines all three representations. The Abstract instantiation
 *     // introduces the virtual slot; Object and Proxy instantiations override it.
 *     template<typename Base>
 *     struct Readable : Base {
 *         using Base::Base;
 *         struct StateTag;
 *
 *         SORA_NO_UNIQUE_ADDRESS InterfaceState<Base::Layer, StateTag> state_;
 *
 *         [[nodiscard]] constexpr virtual int Read() const {
 *             if constexpr (Base::Layer == InterfaceLayer::Abstract) {
 *                 std::terminate(); // The semantic abstract root has no value to dispatch to.
 *             } else {
 *                 ++state_.calls;
 *                 return this->Value().Read();
 *             }
 *         }
 *
 *         [[nodiscard]] constexpr std::size_t ReadCalls() const noexcept
 *             requires(Base::Layer != InterfaceLayer::Abstract)
 *         {
 *             return state_.calls;
 *         }
 *     };
 *
 *     template<typename Base>
 *     struct Incrementable : Base {
 *         using Base::Base;
 *         struct StateTag;
 *
 *         SORA_NO_UNIQUE_ADDRESS InterfaceState<Base::Layer, StateTag> state_;
 *
 *         constexpr virtual void Increment(int delta) {
 *             if constexpr (Base::Layer == InterfaceLayer::Abstract) {
 *                 std::terminate();
 *             } else {
 *                 ++state_.calls;
 *                 this->Value().Increment(delta);
 *             }
 *         }
 *
 *         [[nodiscard]] constexpr std::size_t IncrementCalls() const noexcept
 *             requires(Base::Layer != InterfaceLayer::Abstract)
 *         {
 *             return state_.calls;
 *         }
 *     };
 *
 *     template<typename Base>
 *     struct Resettable : Base {
 *         using Base::Base;
 *         struct StateTag;
 *
 *         SORA_NO_UNIQUE_ADDRESS InterfaceState<Base::Layer, StateTag> state_;
 *
 *         constexpr virtual void Reset() {
 *             if constexpr (Base::Layer == InterfaceLayer::Abstract) {
 *                 std::terminate();
 *             } else {
 *                 ++state_.calls;
 *                 this->Value().Reset();
 *             }
 *         }
 *
 *         [[nodiscard]] constexpr std::size_t ResetCalls() const noexcept
 *             requires(Base::Layer != InterfaceLayer::Abstract)
 *         {
 *             return state_.calls;
 *         }
 *     };
 *
 *     // A facade is still a normal interface: it has a dependency list and a
 *     // virtual operation of its own. It is not an empty marker layer.
 *     template<typename Base>
 *     struct CounterInterface : Base {
 *         using Base::Base;
 *         struct StateTag;
 *
 *         SORA_NO_UNIQUE_ADDRESS InterfaceState<Base::Layer, StateTag> state_;
 *
 *         [[nodiscard]] constexpr virtual int Snapshot() const {
 *             if constexpr (Base::Layer == InterfaceLayer::Abstract) {
 *                 std::terminate();
 *             } else {
 *                 ++state_.calls;
 *                 return this->Value().Snapshot();
 *             }
 *         }
 *
 *         [[nodiscard]] constexpr std::size_t SnapshotCalls() const noexcept
 *             requires(Base::Layer != InterfaceLayer::Abstract)
 *         {
 *             return state_.calls;
 *         }
 *     };
 *
 *     inline constexpr auto ReadableDescriptor = Sora::Meta::DescribeLayeredInterface<^^Readable>();
 *     inline constexpr auto IncrementableDescriptor
 *         = Sora::Meta::DescribeLayeredInterface<^^Incrementable, ReadableDescriptor>();
 *     inline constexpr auto ResettableDescriptor
 *         = Sora::Meta::DescribeLayeredInterface<^^Resettable, ReadableDescriptor>();
 *
 *     // The shared Readable dependency is applied exactly once in each layer.
 *     inline constexpr auto CounterDescriptor
 *         = Sora::Meta::DescribeLayeredInterface<^^CounterInterface, IncrementableDescriptor, ResettableDescriptor>();
 *
 *     using AbstractCounter = Sora::Traits::Extended<AbstractRoot, CounterDescriptor>;
 *     using ExpectedAbstract = CounterInterface<Resettable<Incrementable<Readable<AbstractRoot>>>>;
 *     static_assert(std::same_as<AbstractCounter, ExpectedAbstract>);
 *     static_assert(Sora::Meta::InterfaceLayerOf(^^AbstractCounter) == InterfaceLayer::Abstract);
 *
 *     struct CounterModel {
 *         int value{};
 *
 *         [[nodiscard]] constexpr int Read() const noexcept { return value; }
 *         constexpr void Increment(int delta) noexcept { value += delta; }
 *         constexpr void Reset() noexcept { value = 0; }
 *         [[nodiscard]] constexpr int Snapshot() const noexcept { return value; }
 *     };
 *
 *     // ObjectRoot owns the concrete model. Deducing this preserves cv/ref when
 *     // interface methods obtain the value they dispatch to.
 *     template<typename Model>
 *     struct ObjectRoot : AbstractCounter {
 *         static constexpr InterfaceLayer Layer = InterfaceLayer::Object;
 *         Model model_;
 *
 *         constexpr explicit ObjectRoot(Model model) : model_{std::move(model)} {}
 *
 *         [[nodiscard]] constexpr decltype(auto) Value(this auto&& self) noexcept {
 *             return std::forward_like<decltype(self)>(self.model_);
 *         }
 *     };
 *
 *     // Extends is the reflected higher-order form for composition with Meta algorithms.
 *     inline constexpr auto ExtendCounter = Sora::Meta::Extends<CounterDescriptor>;
 *     using CounterObject = Sora::Meta::InvokeResult<ExtendCounter, ObjectRoot<CounterModel>>;
 *     using ExpectedObject = CounterInterface<Resettable<Incrementable<Readable<ObjectRoot<CounterModel>>>>>;
 *     static_assert(std::same_as<CounterObject, ExpectedObject>);
 *
 *     // ProxyRoot is non-owning. Its Value() returns the abstract target, so the
 *     // same interface bodies perform a second virtual dispatch to CounterObject.
 *     struct ProxyRoot : AbstractCounter {
 *         static constexpr InterfaceLayer Layer = InterfaceLayer::Proxy;
 *         AbstractCounter& target_;
 *
 *         constexpr explicit ProxyRoot(AbstractCounter& target) : target_{target} {}
 *
 *         [[nodiscard]] constexpr AbstractCounter& Value() const noexcept { return target_; }
 *     };
 *
 *     using CounterProxy = Sora::Traits::Extended<ProxyRoot, CounterDescriptor>;
 *     using ExpectedProxy = CounterInterface<Resettable<Incrementable<Readable<ProxyRoot>>>>;
 *     static_assert(std::same_as<CounterProxy, ExpectedProxy>);
 *     static_assert(Sora::Meta::InterfaceLayerOf(^^CounterObject) == InterfaceLayer::Object);
 *     static_assert(Sora::Meta::InterfaceLayerOf(^^CounterProxy) == InterfaceLayer::Proxy);
 *
 *     inline void UseCounter() {
 *         CounterObject object{CounterModel{4}};
 *         CounterProxy proxy{object};
 *         AbstractCounter& counter = proxy;
 *
 *         assert(counter.Read() == 4);
 *         assert(counter.Snapshot() == 4);
 *         counter.Increment(3);
 *         assert(counter.Read() == 7);
 *         counter.Reset();
 *         assert(object.Read() == 0);
 *
 *         assert(object.ReadCalls() == 3 && object.IncrementCalls() == 1 && object.ResetCalls() == 1);
 *         assert(object.SnapshotCalls() == 1);
 *         assert(proxy.ReadCalls() == 2 && proxy.IncrementCalls() == 1 && proxy.ResetCalls() == 1);
 *         assert(proxy.SnapshotCalls() == 1);
 *     }
 *
 * } // namespace CounterExample
 * @endcode
 */
#pragma once

#include <concepts>
#include <cstdint>
#include <meta>
#include <type_traits>

#include <Sora/Platform.h>
#include <Sora/Core/Traits/MetaOperators.h>

namespace Sora {

    /**
     * @brief Representation identity used to scope duplicate-interface detection.
     * @details The values correspond to stdexec's abstract contract, concrete object model, and forwarding proxy
     *     model. An interface inherited from another layer does not satisfy the current layer's implementation check.
     */
    enum class InterfaceLayer : std::uint8_t {
        Abstract, /**< Semantic contract used as the polymorphic base. */
        Object,   /**< Interface implementation dispatching to an owned concrete model. */
        Proxy,    /**< Interface implementation forwarding to another abstract model. */
    };

} // namespace Sora

namespace Sora::Meta {

    /**
     * @brief Compile-time description of one unary interface and its ordered dependencies.
     *
     * @details
     * `interface` reflects a class or alias template that accepts one base
     * type. Every element of `bases` is a reflected constant
     * `LayeredInterfaceDescriptor`. The list is ordered and is applied from first to
     * last before `interface` itself is applied. Consequently, a descriptor is a
     * node in an ordered dependency tree; shared nodes are linearized once by the
     * same-layer implementation check.
     */
    struct LayeredInterfaceDescriptor {
        std::meta::info interface{}; /**< Reflected unary class or alias template. */
        InfoList bases{};            /**< Ordered descriptor constants applied before @p interface. */
    };

    namespace Detail {

        template<typename Type>
        concept LayeredClass
            = std::is_class_v<Type> && requires { typename std::integral_constant<InterfaceLayer, Type::Layer>; };

        template<typename Type, InterfaceLayer Layer>
        concept InLayer = LayeredClass<std::remove_cvref_t<Type>> && std::remove_cvref_t<Type>::Layer == Layer;

        template<typename Extension, typename Base>
        concept LayerExtension = LayeredClass<Extension>
                                 && LayeredClass<Base>
                                 && std::derived_from<Extension, Base>
                                 && Extension::Layer
                                 == Base::Layer;

        [[nodiscard]] consteval bool IsInterfaceTemplate(std::meta::info interface) noexcept {
            return std::meta::is_class_template(interface) || std::meta::is_alias_template(interface);
        }

        // COCA represents a reflected structural NTTP as a const object/value; reject mutable or non-value entities.
        [[nodiscard]] consteval bool IsDescriptorValue(std::meta::info value) noexcept {
            if ((!std::meta::is_object(value) && !std::meta::is_value(value)) || !std::meta::is_const(value)) {
                return false;
            }

            const auto valueType = std::meta::type_of(value);
            return std::meta::is_type(valueType)
                   && std::meta::dealias(std::meta::remove_cvref(valueType))
                   == ^^LayeredInterfaceDescriptor;
        }

        template<template<typename> typename Interface, typename Base>
        [[nodiscard]] constexpr const Interface<Base>& InterfaceCast(const Interface<Base>& value) noexcept {
            return value;
        }

        template<typename Derived, template<typename> typename Interface>
        [[nodiscard]] consteval bool ImplementsInterface() noexcept {
            return requires(const Derived& value) {
                { InterfaceCast<Interface>(value) } -> InLayer<Derived::Layer>;
            };
        }

        template<LayeredClass Type>
        [[nodiscard]] consteval InterfaceLayer LayerOfType() noexcept {
            return Type::Layer;
        }

        template<typename Extension, typename Base>
        [[nodiscard]] consteval bool IsLayerExtension() noexcept {
            return LayerExtension<Extension, Base>;
        }

        template<std::meta::info Function>
        [[nodiscard]] consteval std::meta::info InvokeTemplateFunction(const InfoList& arguments) {
            return InvokeFunction(Apply(Function, arguments));
        }

        [[nodiscard]] consteval std::meta::info LayerOf(std::meta::info type) {
            if (IsMetaError(type)) {
                return type;
            }
            if (std::meta::is_type(type)) {
                type = std::meta::dealias(type);
            }
            return InvokeTemplateFunction<^^LayerOfType>({type});
        }

        [[nodiscard]] consteval std::meta::info ImplementationCheck(std::meta::info core, std::meta::info interface) {
            return InvokeTemplateFunction<^^ImplementsInterface>({core, interface});
        }

        [[nodiscard]] consteval std::meta::info ApplyInterface(std::meta::info core, std::meta::info interface) {
            auto result = Apply(interface, {core});
            if (IsMetaError(result)) {
                return result;
            }
            if (!std::meta::is_type(result)) {
                return VerifyTypeResult(result, {interface, core});
            }

            result = std::meta::dealias(result);
            const auto valid = InvokeTemplateFunction<^^IsLayerExtension>({result, core});
            if (IsMetaError(valid)) {
                return valid;
            }
            return std::meta::extract<bool>(valid) ? result
                                                   : MakeMetaError(^^Error::NonInvocable, {interface, core, result});
        }

        [[nodiscard]] consteval bool IsValidDescriptor(LayeredInterfaceDescriptor desc) {
            if (!IsInterfaceTemplate(desc.interface)) {
                return false;
            }
            for (const auto base : desc.bases) {
                if (!IsDescriptorValue(base)
                    || !IsValidDescriptor(std::meta::extract<LayeredInterfaceDescriptor>(base))) {
                    return false;
                }
            }
            return true;
        }

        /** @brief Error-aware left fold implementing the complete layered-interface extension algebra. */
        class LayeredInterfaceFold {
        public:
            consteval explicit LayeredInterfaceFold(std::meta::info core) : core_{core} {}

            consteval void Apply(LayeredInterfaceDescriptor desc) {
                if (Failed()) {
                    return;
                }
                if (IsMetaError(desc.interface)) {
                    core_ = desc.interface;
                    return;
                }
                if (!IsInterfaceTemplate(desc.interface)) {
                    core_ = MakeMetaError(^^Error::NonInvocable, {desc.interface, core_});
                    return;
                }
                if (!ValidateCore()) {
                    return;
                }

                const auto implementation = ImplementationCheck(core_, desc.interface);
                if (IsMetaError(implementation)) {
                    core_ = implementation;
                    return;
                }

                const bool alreadyImplemented = std::meta::extract<bool>(implementation);
                Apply(desc.bases);
                if (!Failed() && !alreadyImplemented) {
                    core_ = ApplyInterface(core_, desc.interface);
                }
            }

            consteval void Apply(const InfoList& interfaces) {
                for (const auto interface : interfaces) {
                    if (Failed()) {
                        return;
                    }
                    if (IsMetaError(interface)) {
                        core_ = interface;
                    } else if (!IsDescriptorValue(interface)) {
                        core_ = MakeMetaError(^^Error::InvalidSequence, {interface});
                    } else {
                        Apply(std::meta::extract<LayeredInterfaceDescriptor>(interface));
                    }
                }
            }

            [[nodiscard]] consteval std::meta::info Result() const noexcept { return core_; }

        private:
            [[nodiscard]] consteval bool Failed() const noexcept { return IsMetaError(core_); }

            [[nodiscard]] consteval bool ValidateCore() {
                if (coreValidated_) {
                    return true;
                }

                const auto layer = LayerOf(core_);
                if (IsMetaError(layer)) {
                    core_ = layer;
                    return false;
                }

                core_ = std::meta::dealias(core_);
                coreValidated_ = true;
                return true;
            }

            std::meta::info core_;
            bool coreValidated_{};
        };

    } // namespace Detail

    /**
     * @brief Build a validated layered-interface descriptor.
     * @tparam Interface Reflected unary class or alias template.
     * @tparam Bases Interface dependencies in application order.
     * @return A structural descriptor suitable for a non-type template argument.
     * @throws A translation-time diagnostic when @p Interface is not a template or a dependency is malformed.
     */
    template<std::meta::info Interface, LayeredInterfaceDescriptor... Bases>
    consteval LayeredInterfaceDescriptor DescribeLayeredInterface() {
        if (!Detail::IsInterfaceTemplate(Interface) || !(Detail::IsValidDescriptor(Bases) && ...)) {
            throw std::define_static_string(
                "DescribeLayeredInterface requires a template and valid layered-interface dependencies.");
        }

        return {.interface = Interface, .bases = InfoList{std::meta::reflect_constant(Bases)...}};
    }

    /**
     * @brief Validate one descriptor and its complete dependency graph.
     * @param[in] desc Descriptor to validate.
     * @return `true` when every node reflects a template and every dependency is a descriptor value.
     *
     * @details
     * C++26 reflection does not expose a template parameter list. The
     * one-base contract is therefore validated at the first application in
     * `ExtendLayeredInterface`, where `Meta::Apply` can provide the precise
     * substitution result.
     */
    [[nodiscard]] consteval bool ValidateLayeredInterfaceDescriptor(LayeredInterfaceDescriptor desc) {
        return Detail::IsValidDescriptor(desc);
    }

    /**
     * @brief Read the layer identity exposed by a layered root or interface specialization.
     * @param[in] type Reflected class type exposing a static `InterfaceLayer` member named `Layer`.
     * @return The type's layer identity.
     * @throws A translation-time diagnostic when @p type does not satisfy the layer protocol.
     */
    [[nodiscard]] consteval InterfaceLayer InterfaceLayerOf(std::meta::info type) {
        const auto result = Detail::LayerOf(type);
        if (IsMetaError(result)) {
            throw std::define_static_string("InterfaceLayerOf requires a reflected class with a static constexpr "
                                            "InterfaceLayer member named Layer.");
        }

        return std::meta::extract<InterfaceLayer>(result);
    }

    /**
     * @brief Apply an ordered top-level interface sequence by left fold.
     * @param[in] core Reflected initial class type.
     * @param[in] interfaces Reflected descriptor values in source order.
     * @return Reflected final class type or the first `MetaError`.
     * @details An empty @p interfaces sequence is the identity and returns @p core without requiring the layer
     *     protocol. Otherwise, every descriptor is applied to the previous result in source order. Invalid sequence
     *     elements produce `InvalidSequence`; substitution and layer-invariant failures produce `NonInvocable` or
     *     `NonTypeResult`. Existing errors propagate unchanged from left to right.
     * @par Complexity Linear in the visited nodes of the unfolded descriptor trees, excluding compiler substitution
     *     cost. Duplicate interface applications are suppressed, but their descriptor nodes are still visited.
     */
    [[nodiscard]] consteval std::meta::info ExtendLayeredInterfaces(std::meta::info core, const InfoList& interfaces) {
        Detail::LayeredInterfaceFold fold{core};
        fold.Apply(interfaces);
        return fold.Result();
    }

    /**
     * @brief Apply one descriptor with stdexec-compatible ordering and duplicate suppression.
     * @param[in] core Reflected initial class type.
     * @param[in] desc Interface descriptor to apply.
     * @return Reflected class result or the first `MetaError`.
     * @details Let @c present test whether incoming @p core already exposes @p desc.interface in the same layer, and
     *     let @c bases be the ordered recursive extension of @p core by @p desc.bases. The result is @c bases when
     *     @c present is true, otherwise @c desc.interface<bases>. Crucially, @c present is evaluated against the
     *     incoming core before its dependencies are applied, exactly as in stdexec's `__extends`.
     * @par Complexity Linear in the visited nodes of the unfolded descriptor tree, excluding compiler substitution
     *     cost.
     */
    [[nodiscard]] consteval std::meta::info ExtendLayeredInterface(std::meta::info core,
                                                                   LayeredInterfaceDescriptor desc) {
        Detail::LayeredInterfaceFold fold{core};
        fold.Apply(desc);
        return fold.Result();
    }

    /**
     * @brief Carrier for applying a sequence of layered interfaces.
     * @tparam Interfaces Interface descriptors captured by the closure.
     */
    template<LayeredInterfaceDescriptor... Interfaces>
    struct ExtendsCarrier {
        inline static constexpr InfoList Descriptors{std::meta::reflect_constant(Interfaces)...};

        /** @brief Return whether every captured descriptor has a valid dependency graph. */
        [[nodiscard]] static consteval bool IsValid() noexcept {
            return (Detail::IsValidDescriptor(Interfaces) && ...);
        }

        template<typename Core>
        using Apply = InfoType<ExtendLayeredInterfaces(^^Core, Descriptors)>;
    };

    /**
     * @brief Reflect a unary metafunction that applies an ordered interface sequence.
     * @tparam Interfaces Interface descriptors captured by the closure.
     * @return Reflected alias template accepting the initial core type.
     * @details Capturing no interfaces produces the identity metafunction.
     */
    template<LayeredInterfaceDescriptor... Interfaces>
    inline constexpr std::meta::info Extends = ^^ExtendsCarrier<Interfaces...>::template Apply;

} // namespace Sora::Meta

namespace Sora::Traits {

    /**
     * @brief Project ordered layered-interface extension into a C++ type.
     * @tparam Core Initial class type.
     * @tparam Interfaces Interface descriptors in left-fold order.
     * @return Final linearized interface type, or a type-level `MetaError` for a recoverable failure.
     * @details `Extended<Core>` is exactly `Core`, including when @p Core does not implement the layer protocol.
     */
    template<typename Core, Sora::Meta::LayeredInterfaceDescriptor... Interfaces>
    using Extended = Sora::Meta::InfoType<Sora::Meta::ExtendLayeredInterfaces(
        ^^Core, Sora::Meta::InfoList{std::meta::reflect_constant(Interfaces)...})>;

} // namespace Sora::Traits
