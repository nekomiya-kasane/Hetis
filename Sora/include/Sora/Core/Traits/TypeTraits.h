/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility type metadata.
 * @ingroup Core
 *
 * @details Provides compile-time introspection helpers built on C++26 static reflection and standard concepts. Every
 * facility is @c consteval or @c inline @c constexpr oriented, so queries fold to immediate values or pure type
 * computation without runtime cost.
 */
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <ranges>
#include <algorithm>

#include "Sora/Platform.h"

namespace Sora {

    namespace Meta {

        /** @brief Splice a reflected type into a regular C++ type. */
        template<std::meta::info Info>
        using InfoType = [:Info:];

        /**
         * @brief Return the implementation-defined display string for a reflected declaration or type.
         * @param[in] iMeta Reflected entity whose display string is requested.
         * @return Display string reported by the active reflection implementation.
         */
        consteval auto DisplayStringOf(std::meta::info iMeta) {
            return std::meta::display_string_of(iMeta);
        }

        /**
         * @brief Return the source identifier for a reflected declaration.
         * @param[in] iMeta Reflected declaration whose identifier is requested.
         * @return Source identifier reported by the active reflection implementation.
         */
        consteval auto IdentifierOf(std::meta::info iMeta) {
            return std::meta::identifier_of(iMeta);
        }

        /**
         * @brief Return the identifier if available, otherwise the display string for a reflected declaration.
         * @param[in] iMeta Reflected declaration whose identifier or display string is requested.
         * @return Identifier or display string reported by the active reflection implementation.
         */
        consteval auto IdentifierOrDisplayStringOf(std::meta::info iMeta) {
            return std::meta::has_identifier(iMeta) ? IdentifierOf(iMeta) : DisplayStringOf(iMeta);
        }

        /** @cond INTERNAL */
        namespace Detail {

            /** @brief Minimal FNV-1a state for reflection-only ABI discriminators without depending on Hash.h. */
            struct ReflectionDiscriminatorState {
                std::uint64_t value = 14695981039346656037ULL;

                /** @brief Fold @p byte into the discriminator state. */
                constexpr void Feed(std::byte byte) noexcept {
                    value ^= static_cast<std::uint64_t>(byte);
                    value *= 1099511628211ULL;
                }
            };

        } // namespace Detail
        /** @endcond */

        /**
         * @brief Return an ABI-visible discriminator for reflected named variable @p Variable.
         * @todo This is a hack to work around the fact that distinct reflection NTTP specializations share one symbol.
         * When the bug is fixed, this can be removed and the ABI token can be replaced with a simple pointer to the
         * variable reflection.
         */
        template<std::meta::info Variable>
        consteval std::uint64_t ReflectedVariableDiscriminator() {
            static_assert(std::meta::is_variable(Variable), "Variable must reflect a variable declaration.");
            constexpr auto location = std::meta::source_location_of(Variable);
            Detail::ReflectionDiscriminatorState state{};

            auto feedText = [&state](std::string_view text) {
                for (unsigned char character : text) {
                    state.Feed(static_cast<std::byte>(character));
                }
                state.Feed(std::byte{});
            };
            auto feedInteger = [&state](std::uint_least32_t value) {
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    state.Feed(static_cast<std::byte>((value >> shift) & 0xFFU));
                }
            };

            feedText(location.file_name());
            feedText(location.function_name());
            feedText(IdentifierOrDisplayStringOf(Variable));
            feedInteger(location.line());
            feedInteger(location.column());
            return state.value;
        }

        /** @brief ABI token preventing distinct reflection NTTP specializations from sharing one symbol. */
        template<std::meta::info Variable>
        using ReflectedVariableToken =
            std::integral_constant<std::uint64_t, ReflectedVariableDiscriminator<Variable>()>;

        /**
         * @brief Return the parameter list for a reflected function.
         * @param[in] iMeta Reflected function whose parameters are requested.
         * @return A list of parameter reflections.
         */
        consteval auto ParamsOf(std::meta::info iMeta) {
            return std::meta::parameters_of(iMeta);
        }

        /**
         * @brief Return the type of a reflected declaration or entity.
         * @param[in] iMeta Reflected declaration or entity whose type is requested.
         * @return Type reflection reported by the active reflection implementation.
         */
        consteval auto TypeOf(std::meta::info iMeta) {
            return std::meta::type_of(iMeta);
        }

        /**
         * @brief Project a function reflection's parameter list to a list of parameter type infos.
         * @details Throws a compile-time diagnostic naming the offending entity if @p f is not callable.
         * @param[in] f Reflected function whose parameter types are requested.
         * @return A vector of type infos representing the parameter types of @p f.
         */
        consteval std::vector<std::meta::info> ParamTypesOf(std::meta::info f) {
            if (!std::meta::is_function(f)) {
                throw std::define_static_string(
                    "Meta::ParamTypesOf: '" + std::string{std::meta::display_string_of(f)} +
                    "' is not a function reflection — only methods participate in vtable synthesis.");
            }
            return ParamsOf(f) | std::views::transform(std::meta::type_of) | std::ranges::to<std::vector>();
        }

        /**
         * @brief Return the return type of a reflected function.
         * @param[in] f Reflected function whose return type is requested.
         * @return Type info representing the return type of @p f.
         */
        consteval std::meta::info ReturnTypeOf(std::meta::info f) {
            if (!std::meta::is_function(f)) {
                throw std::define_static_string(
                    "Meta::ReturnTypeOf: '" + std::string{std::meta::display_string_of(f)} +
                    "' is not a function reflection — only methods participate in vtable synthesis.");
            }
            return std::meta::return_type_of(f);
        }

        /**
         * @brief Check if a type is a specialization of a given template.
         * @tparam Template The template to check against.
         * @param[in] type The type to check.
         * @return `true` if `type` is a specialization of `Template`, `false` otherwise.
         */
        template<template<typename...> class Template>
        consteval bool IsSpecializationOf(std::meta::info type) {
            if (!std::meta::is_class_type(type) || !std::meta::has_template_arguments(type)) {
                return false;
            }
            return std::meta::template_of(type) == ^^Template;
        }

        /**
         * @brief @c true if @p m reflects a non-static, non-special member function. Filters out constructors,
         * destructors, static member functions, and data members.
         */
        consteval bool IsRegularMethod(std::meta::info m) {
            return std::meta::is_function(m) && !std::meta::is_static_member(m) && !std::meta::is_constructor(m) &&
                   !std::meta::is_destructor(m);
        }

        /**
         * @brief Return whether @p candidate has the same name and full function signature as @p model.
         */
        consteval bool IsSameSignatureMethod(std::meta::info candidate, std::meta::info model) {
            if (!Sora::Meta::IsRegularMethod(candidate) || !std::meta::has_identifier(candidate)) {
                return false;
            }
            if (Sora::Meta::IdentifierOf(candidate) != Sora::Meta::IdentifierOf(model)) {
                return false;
            }
            if (std::meta::return_type_of(candidate) != std::meta::return_type_of(model)) {
                return false;
            }
            if (std::meta::is_const(candidate) != std::meta::is_const(model)) {
                return false;
            }
            if (std::meta::is_volatile(candidate) != std::meta::is_volatile(model)) {
                return false;
            }

            const auto candidateParams = Sora::Meta::ParamTypesOf(candidate);
            const auto modelParams = Sora::Meta::ParamTypesOf(model);
            return candidateParams.size() == modelParams.size() && std::ranges::equal(candidateParams, modelParams);
        }

        /** @brief Materialise @p text as byte values consumable by constexpr hash states. */
        consteval std::vector<std::byte> BytesOf(std::string_view text) {
            return text | std::views::transform([](char c) {
                       return std::bit_cast<std::byte>(static_cast<unsigned char>(c));
                   }) |
                   std::ranges::to<std::vector>();
        }

        template<std::meta::info Invokable>
        consteval bool Invoke(auto&&... args) {
            static_assert(std::meta::is_function(Invokable), "Invokable must reflect a validation function.");
            return [:Invokable:](std::forward<decltype(args)>(args)...);
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Reflection display string for @p T, preserving template arguments where available. */
        template<typename T>
        inline constexpr auto TypeName = std::meta::display_string_of(^^T);

        /** @brief Reflection display string for @p T after removing cv/ref qualifiers and aliases. */
        template<typename T>
        inline constexpr auto DealiasedTypeName =
            std::meta::display_string_of(std::meta::dealias(std::meta::remove_cvref(^^T)));

        /**
         * @brief Smallest unsigned integer type capable of holding a @p Bits-wide value.
         *
         * Selects @c uint8_t / @c uint16_t / @c uint32_t / @c uint64_t. The mixin uses this to size the per-instance
         * counter according to the user's expected reference-graph depth: a UI tree rarely needs more than 65k
         * references, so a 16-bit counter halves the storage cost compared to a default @c uint32_t.
         *
         * @tparam Bits Counter width in bits. Must be one of @c {8, 16, 32, 64}; an unsupported value selects @c
         * uint64_t to fail safe.
         */
        template<size_t Bits = 16>
        using BestSizeType =
            std::conditional_t<(Bits <= 8), std::uint8_t,
                               std::conditional_t<(Bits <= 16), std::uint16_t,
                                                  std::conditional_t<(Bits <= 32), std::uint32_t, std::uint64_t>>>;

        /** @brief Unsigned integer type with at least @p Bits bits. */
        template<size_t Bits>
            requires(std::has_single_bit(Bits) && Bits <= 64)
        using Unsigned = BestSizeType<Bits>;

        /** @brief Signed integer type with at least @p Bits bits. */
        template<size_t Bits>
            requires(std::has_single_bit(Bits) && Bits <= 64)
        using Signed = std::make_signed_t<BestSizeType<Bits>>;

        /** @brief Unsigned integer type with exactly @p Bits bits. */
        template<size_t Bits>
            requires(std::has_single_bit(Bits) && Bits <= 64)
        using Float = std::conditional_t<(Bits <= 16), __bfloat16, std::conditional_t<(Bits <= 32), float, double>>;

    } // namespace Traits

    namespace Concept {

        /** @brief An aggregate type (brace-initialisable, no user-declared ctors). */
        template<typename T>
        concept AggregateType = std::is_aggregate_v<T>;

        /** @brief A standard-layout type (safe for C interop / `offsetof`). */
        template<typename T>
        concept StandardLayoutType = std::is_standard_layout_v<T>;

        /** @brief A trivially copyable type (safe to relocate via `memcpy`). */
        template<typename T>
        concept TriviallyCopyableType = std::is_trivially_copyable_v<T>;

        /** @brief An empty class type (no non-static data members, e.g. a tag). */
        template<typename T>
        concept EmptyType = std::is_empty_v<T>;

        /** @brief A polymorphic class type (declares or inherits a virtual function). */
        template<typename T>
        concept PolymorphicType = std::is_polymorphic_v<T>;

        /**
         * @brief A type whose value representation has no padding bits, so any two
         *        equal values share the same object representation.
         *
         * Such types are safe to hash or compare over their raw bytes (`memcmp`).
         */
        template<typename T>
        concept UniquelyRepresented = std::has_unique_object_representations_v<T>;

        /** @brief Range type whose elements are of type @p E. */
        template<typename T, typename E>
        concept RangeWithElementType = std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, E>;

        /** @brief Type that exposes the standard tuple protocol through @c std::tuple_size and @c std::get. */
        template<typename T>
        concept TupleLikeClass = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return requires(T && t) {
                (std::get<N>(t), ...);
            };
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        /** @brief Type that exposes the standard variant protocol through @c std::variant_size and @c std::get. */
        template<typename T>
        concept VariantLikeClass = requires {
            typename std::variant_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return requires(T && t) {
                (std::get<N>(t), ...);
            };
        }(std::make_index_sequence<std::variant_size_v<std::remove_cvref_t<T>>>{});

        template<typename T>
        concept OptionalLike = requires(T value) {
            typename std::remove_cvref_t<T>::value_type;
            { value.has_value() } -> std::convertible_to<bool>;
            *value;
        };

        /** @brief String-like type (either `std::string` or convertible to `std::string_view`). */
        template<typename T>
        concept StringLike =
            std::same_as<std::remove_cvref_t<T>, std::string> || std::convertible_to<T, std::string_view>;

        /** @brief Scalar field accepted by the stable wire serializer. */
        template<typename T>
        concept IntegerLike = std::integral<T> || std::is_enum_v<T>;

        /** @brief Numeric scalar type (either integer-like or floating-point). */
        template<typename T>
        concept NumericScalar = std::floating_point<std::decay_t<T>> ||
                                (std::integral<std::decay_t<T>> && !std::same_as<std::decay_t<T>, bool>);

        /** @brief Arithmetic scalar type (either integer-like or floating-point). */
        template<typename T>
        concept ArithmeticScalar = std::is_arithmetic_v<T> && !std::same_as<std::remove_cv_t<T>, bool>;

        /** @brief Type that can be efficiently vectorized (arithmetic scalar of size <= 8 bytes). */
        template<typename T>
        concept Vectorizable = std::is_arithmetic_v<T> && !std::same_as<std::remove_cv_t<T>, bool> && sizeof(T) <= 8;

        /** @brief Signed integer-like type. */
        template<typename T>
        concept SignedIntegerLike = IntegerLike<T> && std::is_signed_v<T>;

        /** @brief Unsigned integer-like type. */
        template<typename T>
        concept UnsignedIntegerLike = IntegerLike<T> && std::is_unsigned_v<T>;

        /** @brief Signed arithmetic type. */
        template<typename T>
        concept SignedArithmetic = ArithmeticScalar<T> && std::is_signed_v<T>;

        /** @brief Unsigned arithmetic type. */
        template<typename T>
        concept UnsignedArithmetic = ArithmeticScalar<T> && std::is_unsigned_v<T>;

        /** @brief One-byte type that can be fed into byte-range hash helpers without reinterpret casts. */
        template<typename T>
        concept ByteLike = std::same_as<std::remove_cv_t<T>, std::byte> ||
                           (std::integral<std::remove_cv_t<T>> && sizeof(std::remove_cv_t<T>) == 1);

        /** @brief Floating-point field accepted by the stable wire serializer. */
        template<typename T>
        concept FloatingLike = std::floating_point<T>;

        /** @brief Type that exposes a stable @c view() convertible to @c std::string_view. */
        template<typename T>
        concept StringViewProvider = requires(const std::remove_cvref_t<T>& value) {
            { value.view() } -> std::convertible_to<std::string_view>;
        };

        /** @brief Fixed-string-like structural string carrier with @c view(), @c data(), and @c size(). */
        template<typename T>
        concept FixedStringLike = StringViewProvider<T> && requires(const std::remove_cvref_t<T>& value) {
            { value.data() } -> std::convertible_to<const char*>;
            { value.size() } -> std::convertible_to<size_t>;
        };

        template<typename T>
        concept ByteRange = std::ranges::range<T> && requires { typename std::ranges::range_value_t<T>; } &&
                            (std::same_as<std::remove_cv_t<std::ranges::range_value_t<T>>, std::byte> ||
                             std::same_as<std::remove_cv_t<std::ranges::range_value_t<T>>, unsigned char> ||
                             std::same_as<std::remove_cv_t<std::ranges::range_value_t<T>>, std::uint8_t>);

    } // namespace Concept

    /** @brief Cast an integer-like value to its unsigned representation. */
    [[nodiscard]] constexpr auto CastToUnsigned(Concept::IntegerLike auto value) noexcept {
        if constexpr (std::is_enum_v<decltype(value)>) {
            using U = std::make_unsigned_t<std::underlying_type_t<decltype(value)>>;
            return static_cast<U>(value);
        } else {
            using U = std::make_unsigned_t<decltype(value)>;
            return static_cast<U>(value);
        }
    }

    /** @brief Cast an integer-like value to its signed representation. */
    [[nodiscard]] constexpr auto CastToSigned(Concept::IntegerLike auto&& value) noexcept {
        if constexpr (std::is_enum_v<std::remove_cvref_t<decltype(value)>>) {
            using U = std::make_signed_t<std::underlying_type_t<std::remove_cvref_t<decltype(value)>>>;
            return static_cast<U>(value);
        } else {
            using U = std::make_signed_t<std::remove_cvref_t<decltype(value)>>;
            return static_cast<U>(value);
        }
    }

    namespace Traits {

        /**
         * @brief Pure-type-parameter substitution target for thunk-pointer construction. Specialisations are exactly
         * the flat free-function-pointer types `R(*)(Args...)`, used as the reflection-domain handle for
         * `std::meta::substitute`.
         */
        template<typename Ret, typename... Args>
        using FunctionPointer = Ret (*)(Args...);

        /**
         * @brief Type-level list and combinators
         * @{
         */

        /** @brief A compile-time list of types — the type-level analogue of a tuple. */
        template<typename... Ts>
        struct TypeList {
            static constexpr size_t size = sizeof...(Ts); /**< Number of elements. */

            /** @brief Return a list with @p Us appended without duplicate filtering. */
            template<typename... Us>
            [[nodiscard]] consteval auto Append() const noexcept;

            /** @brief Return a list with @p Us appended and first-occurrence uniqueness preserved. */
            template<typename... Us>
            [[nodiscard]] consteval auto AppendUnique() const noexcept;
        };

        /** @brief Number of elements in @p L. */
        template<template<typename...> class L, typename... Ts>
        inline constexpr size_t Length = L<Ts...>::size;

        /** @cond INTERNAL */
        namespace Detail {

            template<typename L, size_t I>
            struct AtT;
            template<typename... Ts, size_t I>
            struct AtT<TypeList<Ts...>, I> {
                using type = Ts...[I];
            };

            template<typename L>
            struct TailT;
            template<typename T, typename... Ts>
            struct TailT<TypeList<T, Ts...>> {
                using type = TypeList<Ts...>;
            };

            template<typename... Ls>
            consteval std::meta::info ConcatImpl() {
                std::vector<std::meta::info> types;
                (types.append_range(std::meta::template_arguments_of(^^Ls)), ...);
                return std::meta::substitute(^^TypeList, types);
            }

            template<typename... Ls>
            struct ConcatT {
                using type = typename [:ConcatImpl<Ls...>():];
            };
            template<>
            struct ConcatT<> {
                using type = TypeList<>;
            };

            template<template<typename> typename F, typename L>
            struct MapTT;
            template<template<typename> typename F, typename... Ts>
            struct MapTT<F, TypeList<Ts...>> {
                using type = TypeList<F<Ts>...>;
            };

            template<template<typename> typename Pred, typename L>
            struct FilterTT;
            template<template<typename> typename Pred, typename... Ts>
            struct FilterTT<Pred, TypeList<Ts...>> {
                static consteval std::meta::info Compute() {
                    std::vector<std::meta::info> result;
                    const bool keep[] = {Pred<Ts>::value...};
                    auto all = std::meta::template_arguments_of(^^TypeList<Ts...>);
                    for (size_t i = 0; i < all.size(); ++i) {
                        if (keep[i]) {
                            result.push_back(all[i]);
                        }
                    }
                    return std::meta::substitute(^^TypeList, result);
                }
                using type = typename [:Compute():];
            };

            template<template<typename...> typename F, typename L>
            struct ApplyTT;
            template<template<typename...> typename F, typename... Ts>
            struct ApplyTT<F, TypeList<Ts...>> {
                using type = F<Ts...>;
            };

            template<template<typename, typename> typename Op, typename Acc, typename L>
            struct FoldTT;
            template<template<typename, typename> typename Op, typename Acc>
            struct FoldTT<Op, Acc, TypeList<>> {
                using type = Acc;
            };
            template<template<typename, typename> typename Op, typename Acc, typename T, typename... Ts>
            struct FoldTT<Op, Acc, TypeList<T, Ts...>> {
                using type =
                    typename FoldTT<Op, typename [:std::meta::substitute(^^Op, {^^Acc, ^^T}):], TypeList<Ts...>>::type;
            };

            template<typename T, typename... Ts>
            consteval size_t IndexOfImpl() {
                if constexpr (sizeof...(Ts) == 0) {
                    return size_t(-1);
                } else {
                    const bool match[] = {std::is_same_v<T, Ts>...};
                    for (size_t i = 0; i < sizeof...(Ts); ++i) {
                        if (match[i]) {
                            return i;
                        }
                    }
                    return size_t(-1);
                }
            }
            template<typename L, typename T>
            struct IndexOfT;
            template<typename T, typename... Ts>
            struct IndexOfT<TypeList<Ts...>, T> {
                static constexpr size_t value = IndexOfImpl<T, Ts...>();
            };

            template<typename T>
            consteval std::meta::info TypeListReflOf() {
                std::vector<std::meta::info> types;
                types.append_range(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()) |
                                   std::views::transform(std::meta::type_of));
                return std::meta::substitute(^^TypeList, types);
            }

            template<typename L>
            struct LastT;
            template<typename... Ts>
            struct LastT<TypeList<Ts...>> {
                static_assert(sizeof...(Ts) > 0, "Last of an empty TypeList");
                using type = Ts...[sizeof...(Ts) - 1];
            };

            template<typename T, typename L>
            struct PushFrontT;
            template<typename T, typename... Ts>
            struct PushFrontT<T, TypeList<Ts...>> {
                using type = TypeList<T, Ts...>;
            };

            template<typename L, typename T>
            struct PushBackT;
            template<typename T, typename... Ts>
            struct PushBackT<TypeList<Ts...>, T> {
                using type = TypeList<Ts..., T>;
            };

            template<typename L>
            consteval std::meta::info ReverseImpl() {
                auto args = std::meta::template_arguments_of(^^L);
                std::vector<std::meta::info> rev(args.rbegin(), args.rend());
                return std::meta::substitute(^^TypeList, rev);
            }
            template<typename L>
            struct ReverseT {
                using type = typename [:ReverseImpl<L>():];
            };

            template<typename L>
            consteval std::meta::info PopBackImpl() {
                auto args = std::meta::template_arguments_of(^^L);
                if (!args.empty()) {
                    args.pop_back();
                }
                return std::meta::substitute(^^TypeList, args);
            }
            template<typename L>
            struct PopBackT {
                using type = typename [:PopBackImpl<L>():];
            };

            template<typename... Ts>
            consteval std::meta::info UniqueImpl() {
                std::vector<std::meta::info> result;
                auto all = std::meta::template_arguments_of(^^TypeList<Ts...>);
                for (auto arg : all) {
                    bool seen = false;
                    for (auto kept : result) {
                        if (kept == arg) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        result.push_back(arg);
                    }
                }
                return std::meta::substitute(^^TypeList, result);
            }
            template<typename L>
            struct UniqueT;
            template<typename... Ts>
            struct UniqueT<TypeList<Ts...>> {
                using type = typename [:UniqueImpl<Ts...>():];
            };

            template<template<typename> typename Pred, typename L>
            struct PredFoldT;
            template<template<typename> typename Pred, typename... Ts>
            struct PredFoldT<Pred, TypeList<Ts...>> {
                static constexpr bool all = (Pred<Ts>::value && ...);
                static constexpr bool any = (Pred<Ts>::value || ...);
                static constexpr size_t count = (size_t{0} + ... + (Pred<Ts>::value ? size_t{1} : size_t{0}));
            };

            template<template<typename> typename Pred, typename L>
            inline constexpr bool AllOfV = PredFoldT<Pred, L>::all;
            template<template<typename> typename Pred, typename L>
            inline constexpr bool AnyOfV = PredFoldT<Pred, L>::any;
            template<template<typename> typename Pred, typename L>
            inline constexpr size_t CountIfV = PredFoldT<Pred, L>::count;

        } // namespace Detail
        /** @endcond */

        /** @brief The @p I-th element of @p L (C++26 pack indexing). */
        template<typename L, size_t I>
        using At = typename Detail::AtT<L, I>::type;

        /** @brief First element of @p L. */
        template<typename L>
        using Head = At<L, 0>;

        /** @brief All but the first element of @p L. */
        template<typename L>
        using Tail = typename Detail::TailT<L>::type;

        /** @brief Concatenate any number of `TypeList`s. */
        template<typename... Ls>
        using Concat = typename Detail::ConcatT<Ls...>::type;

        /** @brief Apply unary metafunction @p F to each element: `TypeList<F<Ts>...>`. */
        template<template<typename> typename F, typename L>
        using MapT = typename Detail::MapTT<F, L>::type;

        /** @brief Keep elements for which `Pred<T>::value` holds. */
        template<template<typename> typename Pred, typename L>
        using FilterT = typename Detail::FilterTT<Pred, L>::type;

        /** @brief Instantiate variadic template @p F with the list elements: `F<Ts...>`. */
        template<template<typename...> typename F, typename L>
        using ApplyT = typename Detail::ApplyTT<F, L>::type;

        /** @brief Left fold with binary metafunction `Op<Acc, T>` and seed @p Init. */
        template<template<typename, typename> typename Op, typename Init, typename L>
        using FoldT = typename Detail::FoldTT<Op, Init, L>::type;

        /** @brief Index of the first occurrence of @p T in @p L, or `size_t(-1)`. */
        template<typename L, typename T>
        inline constexpr size_t IndexOf = Detail::IndexOfT<L, T>::value;

        /** @brief Whether @p L contains type @p T. */
        template<typename L, typename T>
        inline constexpr bool Contains = (IndexOf<L, T> != size_t(-1));

        /** @brief Last element of @p L (requires a non-empty list). */
        template<typename L>
        using Last = typename Detail::LastT<L>::type;

        /** @brief Prepend @p T to @p L: `TypeList<T, Ts...>`. */
        template<typename T, typename L>
        using PushFront = typename Detail::PushFrontT<T, L>::type;

        /** @brief Append @p T to @p L: `TypeList<Ts..., T>`. */
        template<typename L, typename T>
        using PushBack = typename Detail::PushBackT<L, T>::type;

        /** @brief Remove the last element of @p L (no-op on an empty list). */
        template<typename L>
        using PopBack = typename Detail::PopBackT<L>::type;

        /** @brief Reverse the element order of @p L. */
        template<typename L>
        using Reverse = typename Detail::ReverseT<L>::type;

        /** @brief Remove duplicate elements from @p L, keeping first occurrences. */
        template<typename L>
        using Unique = typename Detail::UniqueT<L>::type;

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::Append() const noexcept {
            return TypeList<Ts..., std::remove_cvref_t<Us>...>{};
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::AppendUnique() const noexcept {
            using Added = TypeList<std::remove_cvref_t<Us>...>;
            using Next = Unique<Concat<TypeList<Ts...>, Added>>;
            return Next{};
        }

        /** @brief `true` if `Pred<T>::value` holds for **every** element of @p L
         *         (vacuously `true` for an empty list).
         */
        template<template<typename> typename Pred, typename L>
        inline constexpr bool AllOf = Detail::AllOfV<Pred, L>;

        /** @brief `true` if `Pred<T>::value` holds for **at least one** element. */
        template<template<typename> typename Pred, typename L>
        inline constexpr bool AnyOf = Detail::AnyOfV<Pred, L>;

        /** @brief `true` if `Pred<T>::value` holds for **no** element. */
        template<template<typename> typename Pred, typename L>
        inline constexpr bool NoneOf = !AnyOf<Pred, L>;

        /** @brief Number of elements of @p L for which `Pred<T>::value` holds. */
        template<template<typename> typename Pred, typename L>
        inline constexpr size_t CountIf = Detail::CountIfV<Pred, L>;

        /** @brief Reflection bridge: a `TypeList` of @p T's data-member types. */
        template<typename T>
        using ToTypeList = [:Detail::TypeListReflOf<T>():];

        /** @} */

    } // namespace Traits

    namespace Meta {

        /** @brief Return whether @p info reflects a @ref Sora::Traits::TypeList specialization. */
        consteval bool IsTypeList(std::meta::info info) {
            return IsSpecializationOf<Sora::Traits::TypeList>(std::meta::dealias(info));
        }

        /** @brief Return the dealiased element-type reflections stored in a reflected @ref Sora::Traits::TypeList. */
        consteval std::vector<std::meta::info> TypeListTypesOf(std::meta::info list) {
            list = std::meta::dealias(list);
            if (!IsTypeList(list)) {
                throw std::define_static_string("Meta::TypeListTypesOf: '" +
                                                std::string{std::meta::display_string_of(list)} +
                                                "' is not a Sora::Traits::TypeList specialization.");
            }
            return std::meta::template_arguments_of(list) | std::views::transform(std::meta::dealias) |
                   std::ranges::to<std::vector>();
        }

        /** @brief Append unique dealiased element-type reflections from @p list to @p out. */
        consteval void AppendTypeListUnique(std::vector<std::meta::info>& out, std::meta::info list) {
            for (auto type : TypeListTypesOf(list)) {
                if (!std::ranges::contains(out, type)) {
                    out.push_back(type);
                }
            }
        }

        /** @brief Append unique dealiased element-type reflections from @p List to @p out. */
        template<typename List>
        consteval void AppendTypeListUnique(std::vector<std::meta::info>& out) {
            AppendTypeListUnique(out, std::meta::dealias(^^List));
        }

    } // namespace Meta

    /** @brief Always returns true. */
    bool AlwaysTrue(auto&&...) {
        return true;
    }

    /** @brief Always returns false. */
    bool AlwaysFalse(auto&&...) {
        return false;
    }

    /** @brief Always returns false, but depends on a type parameter to allow SFINAE. */
    template<typename>
    inline constexpr bool kDependentFalse = false;

    /** @brief Always returns true, but depends on a type parameter to allow SFINAE. */
    template<typename>
    inline constexpr bool kDependentTrue = true;

} // namespace Sora
