/**
 * @file InterfaceRoot.h
 * @brief Runtime roots and common base for layered interface representations.
 * @ingroup Core
 *
 * @details
 * `AbstractInterfaceRoot` terminates the interface-extension algebra without imposing type-erased object management.
 * `ErasedObjectRoot` adds the runtime protocol required by owning and non-owning erased storage. `InterfaceBase`
 * carries the representation layer, declared base-interface closure, and inline-model storage requirement through an
 * interface chain. It does not own storage or prescribe allocation.
 *
 * The storage requirement is the total model size and alignment, including polymorphic overhead. A future erased
 * container compares the complete concrete model against this fixed requirement and allocates externally when the
 * model does not fit. No concrete object type is needed to form an abstract interface.
 *
 * @par Example
 * @code{.cpp}
 * #include <Sora/Core/Polymorphism/InterfaceRoot.h>
 *
 * #include <cassert>
 * #include <cstddef>
 * #include <utility>
 *
 * using Sora::InterfaceLayer;
 * using Sora::MaybeAlign;
 * using Sora::Meta::AbstractInterfaceRoot;
 * using Sora::Meta::InterfaceBase;
 * using Sora::Meta::InterfaceStorageRequirement;
 *
 * template<typename Base>
 * struct Readable : InterfaceBase<Readable, Base> {
 *     using Super = InterfaceBase<Readable, Base>;
 *     using Super::Super;
 *     mutable std::size_t readCount_{};
 *
 *     [[nodiscard]] constexpr virtual int Read() const {
 *         if constexpr (Base::Layer != InterfaceLayer::Abstract) {
 *             ++readCount_;
 *         }
 *         return this->Value().Read();
 *     }
 *
 *     [[nodiscard]] constexpr std::size_t ReadCount() const noexcept
 *         requires(Base::Layer != InterfaceLayer::Abstract)
 *     {
 *         return readCount_;
 *     }
 * };
 *
 * inline constexpr auto ReadableDescriptor = Sora::Meta::DescribeLayeredInterface<Readable>();
 * inline constexpr InterfaceStorageRequirement CounterStorage{
 *     .size = 64,
 *     .alignment = MaybeAlign{16},
 * };
 *
 * template<typename Base>
 * struct Resettable
 *     : InterfaceBase<Resettable, Base, Sora::Meta::ExtendsCarrier<ReadableDescriptor>, CounterStorage> {
 *     using Super
 *         = InterfaceBase<Resettable, Base, Sora::Meta::ExtendsCarrier<ReadableDescriptor>, CounterStorage>;
 *     using Super::Super;
 *
 *     constexpr virtual void Reset() { this->Value().Reset(); }
 * };
 *
 * inline constexpr auto CounterDescriptor = Sora::Meta::DescribeLayeredInterface<Resettable>();
 * using AbstractCounter = Sora::Traits::Extended<AbstractInterfaceRoot, CounterDescriptor>;
 *
 * struct Counter {
 *     int value{};
 *
 *     [[nodiscard]] constexpr int Read() const noexcept { return value; }
 *     constexpr void Reset() noexcept { value = 0; }
 * };
 *
 * template<typename Model>
 * struct ObjectRoot : AbstractCounter {
 *     static constexpr InterfaceLayer Layer = InterfaceLayer::Object;
 *     Model value_;
 *
 *     constexpr explicit ObjectRoot(Model value) : value_{std::move(value)} {}
 *
 *     [[nodiscard]] constexpr decltype(auto) Value(this auto&& self) noexcept {
 *         return std::forward_like<decltype(self)>(self.value_);
 *     }
 * };
 *
 * struct ProxyRoot : AbstractCounter {
 *     static constexpr InterfaceLayer Layer = InterfaceLayer::Proxy;
 *     AbstractCounter& target_;
 *
 *     constexpr explicit ProxyRoot(AbstractCounter& target) : target_{target} {}
 *     [[nodiscard]] constexpr AbstractCounter& Value() const noexcept { return target_; }
 * };
 *
 * using CounterObject = Sora::Traits::Extended<ObjectRoot<Counter>, CounterDescriptor>;
 * using CounterProxy = Sora::Traits::Extended<ProxyRoot, CounterDescriptor>;
 *
 * inline void UseCounter() {
 *     CounterObject object{Counter{7}};
 *     CounterProxy proxy{object};
 *     AbstractCounter& counter = proxy;
 *
 *     assert(counter.Read() == 7);
 *     counter.Reset();
 *     assert(object.Read() == 0);
 *     assert(object.ReadCount() == 2);
 *     assert(proxy.ReadCount() == 1);
 *     static_assert(AbstractCounter::StorageRequirement().size == 64);
 * }
 * @endcode
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <typeinfo>

#include <Sora/Core/Assertion.h>
#include <Sora/Core/Memory/MemoryLayout.h>
#include <Sora/Core/Polymorphism/LayeredInterfaces.h>

namespace Sora::Meta {

    /** @brief Total byte size and alignment reserved for an inline erased model. */
    using InterfaceStorageRequirement = MemorySpanDescriptor;

    struct ObjectAccess;

    /**
     * @brief Terminal root of an abstract layered-interface representation.
     * @details `Value()` preserves the complete cv/ref-qualified interface type so a shared interface body remains
     *     well-formed. Reaching it at runtime means that no object or proxy representation supplied a dispatch target.
     */
    struct AbstractInterfaceRoot {
        static constexpr InterfaceLayer Layer = InterfaceLayer::Abstract;
        using BaseInterfaces = Sora::Meta::ExtendsCarrier<>;

        constexpr virtual ~AbstractInterfaceRoot() = default;

        /** @brief Return the storage occupied by the polymorphic root itself. */
        [[nodiscard]] static consteval InterfaceStorageRequirement StorageRequirement() noexcept {
            return {.size = sizeof(AbstractInterfaceRoot),
                    .alignment = MaybeAlign{static_cast<std::uint64_t>(alignof(AbstractInterfaceRoot))}};
        }

    protected:
        /** @brief Diagnose dispatch through an abstract representation that has no concrete value. */
        template<typename Self>
        [[noreturn]] constexpr Self&& Value(this Self&&) noexcept {
            Unreachable();
        }
    };

    /**
     * @brief Abstract root for representations managed by type-erased storage.
     * @details Object and proxy roots override this protocol. Default bodies diagnose accidental invocation on the
     *     abstract representation while keeping the generated interface type concrete and layout-queryable.
     */
    struct ErasedObjectRoot : AbstractInterfaceRoot {
        /** @brief Return the storage occupied by the erased-object root itself. */
        [[nodiscard]] static consteval InterfaceStorageRequirement StorageRequirement() noexcept {
            return {.size = sizeof(ErasedObjectRoot),
                    .alignment = MaybeAlign{static_cast<std::uint64_t>(alignof(ErasedObjectRoot))}};
        }

    private:
        friend struct ObjectAccess;

        [[nodiscard]] constexpr virtual bool IsObjectEmpty() const noexcept { Unreachable(); }
        constexpr virtual void ResetObject() noexcept { Unreachable(); }
        [[nodiscard]] constexpr virtual const std::type_info& ObjectType() const noexcept { Unreachable(); }
        [[nodiscard]] constexpr virtual void* ObjectData() noexcept { Unreachable(); }
        [[nodiscard]] constexpr virtual const void* ObjectData() const noexcept { Unreachable(); }
        constexpr virtual void DeleteObject() noexcept { Unreachable(); }
    };

    /**
     * @brief CPO implementation family for the erased-object root protocol.
     * @details Each operation is a generic function object, matching stdexec's `__access` shape. The customization
     * contract is the corresponding member function on the root type; no `tag_invoke` or inheritance is required by
     * the CPO itself.
     */
    struct ObjectAccess {
        struct EmptyCpo {
            template<typename Root>
                requires std::derived_from<std::remove_cvref_t<Root>, ErasedObjectRoot>
            [[nodiscard]] constexpr bool operator()(const Root& root) const noexcept {
                return static_cast<const ErasedObjectRoot&>(root).IsObjectEmpty();
            }
        };

        struct ResetCpo {
            template<typename Root>
                requires std::derived_from<Root, ErasedObjectRoot>
            constexpr void operator()(Root& root) const noexcept {
                static_cast<ErasedObjectRoot&>(root).ResetObject();
            }
        };

        struct TypeCpo {
            template<typename Root>
                requires std::derived_from<std::remove_cvref_t<Root>, ErasedObjectRoot>
            [[nodiscard]] constexpr const std::type_info& operator()(const Root& root) const noexcept {
                return static_cast<const ErasedObjectRoot&>(root).ObjectType();
            }
        };

        struct DataCpo {
            template<typename Root>
                requires std::derived_from<std::remove_cvref_t<Root>, ErasedObjectRoot>
            [[nodiscard]] constexpr auto operator()(Root& root) const noexcept {
                if constexpr (std::is_const_v<Root>) {
                    return static_cast<const ErasedObjectRoot&>(root).ObjectData();
                } else {
                    return static_cast<ErasedObjectRoot&>(root).ObjectData();
                }
            }
        };

        struct DeleteCpo {
            template<typename Root>
                requires std::derived_from<Root, ErasedObjectRoot>
            constexpr void operator()(Root& root) const noexcept {
                static_cast<ErasedObjectRoot&>(root).DeleteObject();
            }
        };
    };

    inline constexpr auto ObjectEmpty = ObjectAccess::EmptyCpo{};
    inline constexpr auto ResetObject = ObjectAccess::ResetCpo{};
    inline constexpr auto ObjectType = ObjectAccess::TypeCpo{};
    inline constexpr auto ObjectData = ObjectAccess::DataCpo{};
    inline constexpr auto DeleteObject = ObjectAccess::DeleteCpo{};

    /**
     * @brief Default total inline-model capacity used by an interface that does not request another layout.
     * @details This matches stdexec's effective default of three payload pointers plus one polymorphic-root pointer,
     *     while expressing total model storage directly instead of relying on an implicit vptr-size adjustment.
     */
    inline constexpr InterfaceStorageRequirement kDefaultInterfaceStorage{
        .size = sizeof(AbstractInterfaceRoot) + 3 * sizeof(void*),
        .alignment = MaybeAlign{static_cast<std::uint64_t>(alignof(AbstractInterfaceRoot))},
    };

    /** @cond INTERNAL */
    struct InterfaceDescriptorProbe : AbstractInterfaceRoot {};
    /** @endcond */

} // namespace Sora::Meta

namespace Sora::Meta {

    /**
     * @brief Describe an interface whose dependency closure is declared by @ref InterfaceBase.
     * @tparam Interface Unary layered-interface template.
     * @return A validated descriptor using the interface's inherited `BaseInterfaces` closure.
     * @details This overload makes the interface definition the single source of truth for its dependencies. Use the
     *     reflected overload from `LayeredInterfaces.h` for templates that deliberately do not use `InterfaceBase`.
     */
    template<template<typename> typename Interface>
    consteval LayeredInterfaceDescriptor DescribeLayeredInterface() {
        using Specialization = Interface<InterfaceDescriptorProbe>;
        if constexpr (!requires { typename Specialization::BaseInterfaces; }) {
            throw std::define_static_string(
                "DescribeLayeredInterface<Interface>() requires Interface to derive from Meta::InterfaceBase.");
        } else {
            using Bases = typename Specialization::BaseInterfaces;
            if constexpr (!requires {
                              { Bases::Descriptors } -> std::same_as<const InfoList&>;
                              { Bases::IsValid() } -> std::same_as<bool>;
                          }) {
                throw std::define_static_string(
                    "InterfaceBase dependencies must be represented by Meta::ExtendsCarrier.");
            } else if constexpr (!Bases::IsValid()) {
                throw std::define_static_string(
                    "InterfaceBase contains an invalid layered-interface dependency descriptor.");
            } else {
                return {.interface = ^^Interface, .bases = Bases::Descriptors};
            }
        }
    }

} // namespace Sora::Meta

namespace Sora::Concept {

    /** @brief Ordered dependency closure accepted by a layered interface definition. */
    template<typename Type>
    concept LayeredInterfaceDependencies = requires {
        { Type::Descriptors } -> std::same_as<const Meta::InfoList&>;
        { Type::IsValid() } -> std::same_as<bool>;
    } && Type::IsValid();

    /** @brief Type that can serve as the incoming core of a layered interface specialization. */
    template<typename Type>
    concept LayeredInterfaceCore = std::is_class_v<Type> && requires {
        typename std::integral_constant<InterfaceLayer, Type::Layer>;
        typename Type::BaseInterfaces;
        requires LayeredInterfaceDependencies<typename Type::BaseInterfaces>;
        { Type::StorageRequirement() } -> std::same_as<Meta::InterfaceStorageRequirement>;
    };

} // namespace Sora::Concept

namespace Sora::Meta {

    /**
     * @brief Common base that preserves one layered interface's compile-time representation contract.
     * @tparam Interface Unary interface template being defined.
     * @tparam Base Previously linearized interface core.
     * @tparam Bases Ordered closure describing the interface's declared dependencies.
     * @tparam Storage Total inline-model storage requested by this interface.
     *
     * @details
     * The resulting type remains in @p Base's representation layer. Storage requirements are merged component-wise
     * with the incoming core, so every abstract, object, and proxy linearization observes one stable capacity and
     * alignment. The type owns neither an object nor a buffer; those responsibilities belong to representation roots
     * and erased containers respectively.
     */
    template<template<typename> typename Interface, Concept::LayeredInterfaceCore Base,
             Concept::LayeredInterfaceDependencies Bases = Sora::Meta::ExtendsCarrier<>,
             InterfaceStorageRequirement Storage = kDefaultInterfaceStorage>
    struct InterfaceBase : Base {
        static_assert(Storage.alignment.Encode() <= 64,
                      "InterfaceBase storage alignment must be undefined or a representable power of two.");

        static constexpr InterfaceLayer Layer = Base::Layer;
        using BaseInterfaces = Bases;
        using AbstractInterface = Interface<typename Bases::template Apply<AbstractInterfaceRoot>>;
        using Base::Base;

        /** @brief Return the strongest size and alignment requirement accumulated through the interface chain. */
        [[nodiscard]] static consteval InterfaceStorageRequirement StorageRequirement() {
            return MergeMemorySpanDescriptor({Base::StorageRequirement(), Storage});
        }
    };

} // namespace Sora::Meta
