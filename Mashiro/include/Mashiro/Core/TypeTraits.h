/**
 * @file TypeTraits.h
 * @brief Reflection-based type traits, structural concepts, and utility types.
 *
 * Provides compile-time introspection helpers built on C++26 static reflection
 * (`<meta>`) and standard concepts. Key facilities:
 * - `Homogeneous<T>` — all non-static data members share the same type.
 * - `TupleLike<T>` / `VariantLike<T>` — structural duck-type detection.
 * - `Overload` — lambda-overload-set builder for `std::visit`.
 * - `Anno::*` — annotation probing + member filtering/ordering driven by
 *   user-supplied `Ignore` / `Key` / `Order` annotation tag types.
 * - Standard-library categorisation: `StdOptional`, `StdVariant`,
 *   `ChronoDuration`, `ChronoTimePoint`, `FilesystemPath`, `ByteRange`,
 *   `StringViewConvertible`, `StringKeyedAssociative`.
 *
 * @ingroup Core
 */
#pragma once

#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <meta>
#include <new>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Mashiro {

    namespace Traits {

        namespace Detail {

            /// @brief Compile-time human-readable name of @p T.
            template<typename T>
            [[nodiscard]] consteval std::string_view GetTypeName() {
                return std::meta::display_string_of(^^T);
            }

        }

        /// @brief string_view name of a type.
        template<typename T>
        inline constexpr auto TypeName = Detail::GetTypeName<T>();

        consteval auto DisplayStringOf(std::meta::info iMeta) {
            return std::meta::display_string_of(iMeta);
        }

        /// @brief identifier of
        consteval auto IdentifierOf(std::meta::info iMeta) {
            return std::meta::identifier_of(iMeta);
        }

        /// @brief Static reflection array of @p T's non-static data members.
        template<typename T>
        inline constexpr auto Members = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));

        /// @brief Number of non-static data members in @p T.
        template<typename T>
        inline constexpr size_t MembersCount = Members<T>.size();

        /// @brief Static reflection array of @p T's public non-static data members.
        template<typename T>
        inline constexpr auto PublicMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));

        /// @brief Number of public non-static data members in @p T.
        template<typename T>
        inline constexpr size_t PublicMembersCount = PublicMembers<T>.size();

        namespace Detail {

            /// @brief Total byte size of all data members (may differ from sizeof due to padding).
            template<typename T>
            consteval size_t MemberBytesTotal() {
                size_t total = 0;
                for (auto m : Mashiro::Traits::Members<T>) {
                    total += std::meta::size_of(std::meta::type_of(m));
                }
                return total;
            }

        }

        /// @brief Total byte size of all data members (may differ from sizeof due to padding).
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t MemberBytesTotal = Detail::MemberBytesTotal<T>();
    
        /// @brief Padding bytes = sizeof(T) - sum of member sizes.
        template<typename T> requires std::is_class_v<T>
        inline constexpr size_t PaddingBytes = sizeof(T) - MemberBytesTotal<T>;

        /// @brief Compact class: all members are homogeneous and there's no padding.
        template<typename T>
        concept Compact = std::is_class_v<T> && MembersCount<T> > 0 && PaddingBytes<T> == 0;

        // clang-format off

        namespace Detail {

            /// @brief Compile-time check: every NSDM of @p T has the same type.
            template<typename T>
            consteval bool IsAllMemberHomogeneous() {
                if (MembersCount<T> == 0) {
                    return false;
                }
                auto first_type = type_of(Members<T>[0]);
                for (auto m : Members<T>) {
                    if (type_of(m) != first_type) {
                        return false;
                    }
                }
                return true;
            }

        } // namespace Detail

        /**
        * @brief Concept: all non-static data members of @p T share the same type.
        *
        * Requires @p T to be a class with at least one NSDM, and uses P2996
        * reflection to verify type homogeneity.
        */
        template<typename T>
        concept Homogeneous = std::is_class_v<T> && Detail::IsAllMemberHomogeneous<T>();

        // clang-format on

        /// @name Tuple / variant structural concepts
        /// @{

        /**
         * @brief True if `std::get<N>(t)` is valid and returns the expected element type.
         * @tparam T Type to probe.
         * @tparam N Zero-based element index.
         */
        template<typename T, size_t N>
        concept HasTupleElement = requires(T&& t) {
            typename std::tuple_element_t<N, std::remove_cvref_t<T>>;
            { std::get<N>(t) } -> std::convertible_to<const std::tuple_element_t<N, T>&>;
        };

        /// @brief A type that provides `std::tuple_size` and per-index `std::get`.
        template<typename T>
        concept TupleLike = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::value_type;
        } && []<size_t... N>(std::index_sequence<N...>) {
            return (HasTupleElement<T, N> && ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});

        /**
         * @brief Detected `std::variant` specialisation.
         *
         * Uses `std::variant_size`, which is only specialised for `std::variant`.
         */
        template<typename T>
        concept VariantLike = requires { std::variant_size<std::remove_cvref_t<T>>::value; };

        /// @}

        /**
         * @brief Overload-set builder for `std::visit` and general dispatch.
         *
         * Inherits `operator()` from each callable in @p Ts, producing a single
         * callable object. The CTAD guide allows direct brace-list construction.
         *
         * @tparam Ts Callable types (typically lambdas).
         *
         * @code
         * std::variant<int, std::string, float> v = std::string{"hi"};
         * auto label = std::visit(Overload{
         *     [](int i)                { return "int:"   + std::to_string(i); },
         *     [](const std::string& s) { return "str:"   + s; },
         *     [](auto&&)               { return std::string{"other"}; },
         * }, v);
         * // label == "str:hi"
         * @endcode
         */
        template<typename... Ts>
        struct Overload : Ts... {
            using Ts::operator()...;
        };
        /// @brief CTAD guide — deduce @c Ts from constructor arguments.
        template<typename... Ts>
        Overload(Ts...) -> Overload<Ts...>;

        // ==========================================================================
        // Enum Reflection
        // ==========================================================================

        /// @brief Static reflection array of @p T's enumerators.
        template<typename T>
        inline constexpr auto Enumerators = std::define_static_array(std::meta::enumerators_of(^^T));

        /// @brief Number of enumerators in @p T.
        template<typename T>
        inline constexpr size_t EnumeratorsCount = Enumerators<T>.size();

        namespace Detail {

            /// @brief Unsigned version of an enum's underlying type.
            template<typename E>
                requires std::is_enum_v<E>
            using UnsignedUnderlying = std::make_unsigned_t<std::underlying_type_t<E>>;

            /// @brief Compile-time check: every enumerator is 0 or a power of 2.
            template <typename E>
            consteval bool AllPowerOfTwo() {
                template for (constexpr auto e : Enumerators<E>) {
                    auto v = static_cast<UnsignedUnderlying<E>>([:e:]);
                    if (v != 0 && !std::has_single_bit(v) && (std::meta::display_string_of(e) != "None"))
                        return false;
                }
                return true;
            }

            /// @brief Compile-time check: the enum values increases from 0 within the enum type
            template<typename T>
                requires std::is_enum_v<T>
            consteval bool IsEnumSequential() {
                if (EnumeratorsCount<T> == 0) {
                    return false;
                }
                using U = std::underlying_type_t<T>;
                for (size_t i = 0; i < EnumeratorsCount<T>; ++i) {
                    if (static_cast<U>(std::meta::extract<T>(Enumerators<T>[i])) !=
                        static_cast<U>(i)) {
                        return false;
                    }
                }
                return true;
            }

            /// @brief Compile-time OR of all enumerators — the valid-bit mask.
            template <typename E>
            consteval UnsignedUnderlying<E> AllBitsMask() {
                UnsignedUnderlying<E> mask{};
                template for (constexpr auto e : Enumerators<E>) {
                    mask |= static_cast<UnsignedUnderlying<E>>([:e:]);
                }
                return mask;
            }

        } // namespace Detail

        /**
         * @brief A scoped enum whose enumerators form a contiguous sequence starting from 0.
         *
         * Validated at compile time via P2996 static reflection. Useful for
         * enum-to-index mapping and array indexing by enum value.
         */
        template<typename T>
        concept SequentialEnum = std::is_enum_v<T> && Detail::IsEnumSequential<T>();

        /**
         * @brief A scoped enum whose enumerators are all 0 or exact powers of two.
         *
         * Validated at compile time via P2996 static reflection. Any `enum class`
         * satisfying this concept automatically gets bitwise operators and query
         * functions — zero opt-in required.
         */
        template <typename E>
        concept BitfieldEnum = std::is_enum_v<E> && Detail::AllPowerOfTwo<E>();
        
        /// @brief All-valid-bits mask for a BitfieldEnum (OR of all enumerators).
        template <BitfieldEnum E>
        inline constexpr auto kBitfieldMask = static_cast<E>(Detail::AllBitsMask<E>());

    } // namespace Traits

    namespace Platform {

        /// @brief Cache line size for false-sharing avoidance (`alignas(kCacheLineSize)`).
#ifdef __cpp_lib_hardware_interference_size
        inline constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
        inline constexpr size_t kCacheLineSize = 64;
#endif

    } // namespace Platform

    namespace Traits {

        // =====================================================================
        // Type-level list and combinators
        // =====================================================================

        /// @brief A compile-time list of types — the type-level analogue of a tuple.
        template<typename... Ts>
        struct TypeList {
            static constexpr size_t size = sizeof...(Ts); ///< Number of elements.
        };

        /// @brief Number of elements in @p L.
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
                auto collect = [&](std::meta::info listType) {
                    for (auto arg : std::meta::template_arguments_of(listType))
                        types.push_back(arg);
                };
                (collect(^^Ls), ...);
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
                        if (keep[i])
                            result.push_back(all[i]);
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
                using type = typename FoldTT<Op, typename [:std::meta::substitute(^^Op, {^^Acc, ^^T}):], TypeList<Ts...>>::type;
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
                for (auto m : std::meta::nonstatic_data_members_of(
                         ^^T, std::meta::access_context::unchecked())) {
                    types.push_back(std::meta::type_of(m));
                }
                return std::meta::substitute(^^TypeList, types);
            }

        } // namespace Detail
        /** @endcond */

        /// @brief The @p I-th element of @p L (C++26 pack indexing).
        template<typename L, size_t I>
        using At = typename Detail::AtT<L, I>::type;

        /// @brief First element of @p L.
        template<typename L>
        using Head = At<L, 0>;

        /// @brief All but the first element of @p L.
        template<typename L>
        using Tail = typename Detail::TailT<L>::type;

        /// @brief Concatenate any number of `TypeList`s.
        template<typename... Ls>
        using Concat = typename Detail::ConcatT<Ls...>::type;

        /// @brief Apply unary metafunction @p F to each element: `TypeList<F<Ts>...>`.
        template<template<typename> typename F, typename L>
        using MapT = typename Detail::MapTT<F, L>::type;

        /// @brief Keep elements for which `Pred<T>::value` holds.
        template<template<typename> typename Pred, typename L>
        using FilterT = typename Detail::FilterTT<Pred, L>::type;

        /// @brief Instantiate variadic template @p F with the list elements: `F<Ts...>`.
        template<template<typename...> typename F, typename L>
        using ApplyT = typename Detail::ApplyTT<F, L>::type;

        /// @brief Left fold with binary metafunction `Op<Acc, T>` and seed @p Init.
        template<template<typename, typename> typename Op, typename Init, typename L>
        using FoldT = typename Detail::FoldTT<Op, Init, L>::type;

        /// @brief Index of the first occurrence of @p T in @p L, or `size_t(-1)`.
        template<typename L, typename T>
        inline constexpr size_t IndexOf = Detail::IndexOfT<L, T>::value;

        /// @brief Whether @p L contains type @p T.
        template<typename L, typename T>
        inline constexpr bool Contains = (IndexOf<L, T> != size_t(-1));

        /// @brief Reflection bridge: a `TypeList` of @p T's data-member types.
        template<typename T>
        using ToTypeList = [:Detail::TypeListReflOf<T>():];

        // =====================================================================
        // Annotation helpers
        // =====================================================================

        /**
         * @brief Generic helpers for probing and extracting C++26 annotations.
         *
         * Subsystems define their own annotation tag types (e.g.
         * `Hashing::Anno::Ignore`, `Json::Anno::Ignore`) and parameterise these
         * helpers with them; the helpers themselves are entirely subsystem-
         * agnostic. All operations are `consteval` so they fold to immediate
         * values at the call site.
         *
         * @code
         * struct MyIgnore {};
         * struct My { [[=MyIgnore{}]] int hidden; int kept; };
         * static_assert( Mashiro::Traits::Anno::Has<MyIgnore>(My's hidden member) );
         * @endcode
         */
        namespace Anno {

            /// @brief `true` if @p ent carries any annotation of type @p A.
            template <typename A>
            consteval bool Has(std::meta::info ent) {
                return std::meta::annotations_of(ent, ^^A).size() > 0;
            }

            /// @brief Extract the first annotation of type @p A on @p ent, or `nullopt`.
            template <typename A>
            consteval std::optional<A> Get(std::meta::info ent) {
                auto annots = std::meta::annotations_of(ent, ^^A);
                if (annots.size() > 0) return std::meta::extract<A>(annots[0]);
                return std::nullopt;
            }

            /// @brief `true` if any non-static data member of @p T carries
            ///        annotation @p A (the "whitelist" / Key-mode trigger).
            template <typename T, typename A>
            consteval bool AnyMemberHas() {
                for (auto m : std::meta::nonstatic_data_members_of(
                                  ^^T, std::meta::access_context::unchecked()))
                    if (Has<A>(m)) return true;
                return false;
            }

            /**
             * @brief Filter and stably re-order @p T's non-static data members
             *        for serialisation-style traversal.
             *
             * @tparam T Reflectable class.
             * @tparam Ignore Tag type whose presence excludes a member.
             * @tparam Key   Tag type whose presence on *any* member engages
             *               whitelist mode (only Key-tagged members survive).
             * @tparam Order Tag type with a `priority` member (`int`) used to
             *               sort the surviving members in ascending order;
             *               members without `Order` sort after those with it
             *               (priority = INT_MAX), preserving declaration
             *               order within each tier.
             *
             * @return A `std::vector<std::meta::info>` of selected members in
             *         emission order. The result is intended to be wrapped in
             *         `std::define_static_array(...)` at the call site for use
             *         with `template for`.
             */
            template <typename T, typename Ignore, typename Key, typename Order>
            consteval std::vector<std::meta::info> SelectMembers() {
                auto all = std::meta::nonstatic_data_members_of(
                    ^^T, std::meta::access_context::unchecked());
                const bool keyMode = AnyMemberHas<T, Key>();
                std::vector<std::meta::info> result;
                for (auto m : all) {
                    if (Has<Ignore>(m)) continue;
                    if (keyMode && !Has<Key>(m)) continue;
                    result.push_back(m);
                }
                constexpr int kSentinel = 0x7FFFFFFF;
                auto priorityOf = [](std::meta::info m) -> int {
                    auto o = Get<Order>(m);
                    return o ? o->priority : kSentinel;
                };
                for (size_t i = 1; i < result.size(); ++i) {
                    auto key = result[i];
                    int  pk  = priorityOf(key);
                    size_t j = i;
                    while (j > 0 && priorityOf(result[j - 1]) > pk) {
                        result[j] = result[j - 1];
                        --j;
                    }
                    result[j] = key;
                }
                return result;
            }

            /**
             * @brief Extract a typed payload from a templated annotation
             *        (e.g. `Rename<FixedString>`).
             *
             * Specialise the primary template on the user's templated annotation
             * to expose `value = true` and a `static constexpr` payload — see
             * the @ref Mashiro::Json::Detail::RenameTrait pattern for a worked
             * example. This trait is provided here as a convenience anchor for
             * subsystems that all share the same NTTP-as-annotation pattern.
             */
            template <typename T>
            struct PayloadTrait {
                static constexpr bool value = false;
            };

        } // namespace Anno

        // =====================================================================
        // Standard-library categorisation
        // =====================================================================

        /// @brief A `std::optional<U>` specialisation.
        template <typename T> struct IsStdOptionalT : std::false_type {};
        template <typename U> struct IsStdOptionalT<std::optional<U>> : std::true_type {};
        template <typename T> concept StdOptional =
            IsStdOptionalT<std::remove_cvref_t<T>>::value;

        /// @brief A `std::variant<...>` specialisation.
        template <typename T> struct IsStdVariantT : std::false_type {};
        template <typename... U> struct IsStdVariantT<std::variant<U...>> : std::true_type {};
        template <typename T> concept StdVariant =
            IsStdVariantT<std::remove_cvref_t<T>>::value;

        /// @brief A `std::chrono::duration<Rep, Period>`.
        template <typename T>
        concept ChronoDuration = requires {
            typename T::rep; typename T::period;
            requires std::same_as<std::remove_cvref_t<T>,
                std::chrono::duration<typename T::rep, typename T::period>>;
        };

        /// @brief A `std::chrono::time_point<Clock, Duration>`.
        template <typename T>
        concept ChronoTimePoint = requires {
            typename T::clock; typename T::duration;
            requires std::same_as<std::remove_cvref_t<T>,
                std::chrono::time_point<typename T::clock, typename T::duration>>;
        };

        /// @brief A `std::filesystem::path`.
        template <typename T>
        concept FilesystemPath =
            std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;

        /// @brief A range whose element type is `std::byte` (binary blob).
        template <typename T>
        concept ByteRange = std::ranges::range<T> &&
                            std::same_as<std::ranges::range_value_t<T>, std::byte>;

        /// @brief Convertible to `std::string_view` and not a path.
        ///
        /// `filesystem::path` is convertible-to-string-view on libstdc++ but
        /// has its own dedicated path; exclude it here so dispatchers can
        /// reach the path branch first.
        template <typename T>
        concept StringViewConvertible =
            std::convertible_to<std::remove_cvref_t<T>, std::string_view> &&
            !std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;

        /// @brief An associative range whose key type is convertible to
        ///        `std::string_view` (i.e. encodes naturally as a JSON object).
        template <typename T>
        concept StringKeyedAssociative = std::ranges::range<T> &&
            requires { typename std::remove_cvref_t<T>::key_type; } &&
            std::convertible_to<typename std::remove_cvref_t<T>::key_type,
                                std::string_view>;

    }

} // namespace Mashiro
