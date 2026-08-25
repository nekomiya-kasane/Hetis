/**
 * @file TypeList.h
 * @brief C++26 reflection-native type-list algebra.
 * @ingroup Core
 *
 * @details `TypeList<Ts...>` is the framework's type-level sequence. Its
 * operations are expressed as immediate reflection transforms over the
 * sequence's template arguments. This keeps the public surface small and
 * removes recursive helper specializations from the implementation.
 *
 * @code{.cpp}
 * using Input = Sora::Traits::TypeList<int, float>;
 * using Pointers = Sora::Traits::MapT<std::add_pointer_t, Input>;
 * static_assert(std::same_as<Pointers, Sora::Traits::TypeList<int*, float*>>);
 * @endcode
 */
#pragma once

#include <cstddef>
#include <algorithm>
#include <meta>
#include <ranges>
#include <type_traits>
#include <vector>

#include <Sora/Core/Traits/MetaInfo.h>
#include <Sora/Core/Traits/MetaPredicate.h>

namespace Sora {

    namespace Traits {

        /** @brief A compile-time sequence of types. */
        template<typename... Ts>
        struct TypeList {
            /** @brief Sentinel returned when a type cannot be found. */
            static constexpr std::size_t npos = static_cast<std::size_t>(-1);

            static constexpr std::size_t size = sizeof...(Ts);
            static constexpr bool empty = size == 0;

            /** @brief Return the type at zero-based index @p I. */
            template<std::size_t I>
                requires(I < sizeof...(Ts))
            using At = Ts...[I];

            /** @brief Return the first type; instantiation requires a non-empty list. */
            using Front = Meta::InfoType<[] consteval {
                if constexpr (sizeof...(Ts) == 0) {
                    return ^^void;
                } else {
                    return ^^Ts...[0];
                }
            }()>;

            /** @brief Return the final type; instantiation requires a non-empty list. */
            using Back = Meta::InfoType<[] consteval {
                if constexpr (sizeof...(Ts) == 0) {
                    return ^^void;
                } else {
                    return ^^Ts...[sizeof...(Ts) - 1];
                }
            }()>;

            /** @brief Return the first index containing @p T, or `size_t(-1)`. */
            template<typename T>
            [[nodiscard]] static consteval std::size_t IndexOf() noexcept;

            /** @brief Return whether @p T occurs in this sequence. */
            template<typename T>
            [[nodiscard]] static consteval bool Contains() noexcept;

            /** @brief Return the number of occurrences of @p T. */
            template<typename T>
            [[nodiscard]] static consteval std::size_t Count() noexcept;

            /** @brief Return whether every element type occurs exactly once. */
            [[nodiscard]] static consteval bool IsUnique() noexcept;

            /** @brief Append @p Us without removing duplicates. */
            template<typename... Us>
            [[nodiscard]] consteval auto Concat() const noexcept;

            /** @brief Prepend @p Us without removing duplicates. */
            template<typename... Us>
            [[nodiscard]] consteval auto Prepend() const noexcept;

            /** @brief Prepend @p Us and retain first occurrences only. */
            template<typename... Us>
            [[nodiscard]] consteval auto PrependUnique() const noexcept;

            /** @brief Append @p Us and retain first occurrences only. */
            template<typename... Us>
            [[nodiscard]] consteval auto AppendUnique() const noexcept;

            /** @brief Append @p Us without removing duplicates. */
            template<typename... Us>
            [[nodiscard]] consteval auto Append() const noexcept;

            /** @brief Return an empty type list. */
            [[nodiscard]] consteval auto Clear() const noexcept;

            /** @brief Return all but the first element; an empty list remains empty. */
            [[nodiscard]] consteval auto PopFront() const noexcept;

            /** @brief Return all but the final element; an empty list remains empty. */
            [[nodiscard]] consteval auto PopBack() const noexcept;

            /** @brief Return the elements in reverse order. */
            [[nodiscard]] consteval auto Reverse() const noexcept;

            /** @brief Remove duplicates while retaining first occurrences. */
            [[nodiscard]] consteval auto Unique() const noexcept;
        };

    } // namespace Traits

    namespace Meta {

        /** @brief Return whether @p info reflects a `Traits::TypeList` specialization. */
        [[nodiscard]] consteval bool IsTypeList(std::meta::info info) noexcept {
            info = std::meta::dealias(info);
            return std::meta::is_class_type(info) && std::meta::has_template_arguments(info) &&
                   std::meta::template_of(info) == ^^Traits::TypeList;
        }

        /** @brief Return dealiased element-type reflections from a reflected type list. */
        [[nodiscard]] consteval std::vector<std::meta::info> TypeListTypesOf(std::meta::info list) {
            list = std::meta::dealias(list);
            if (!IsTypeList(list)) {
                throw std::define_static_string("Meta::TypeListTypesOf requires a Sora::Traits::TypeList.");
            }
            return std::meta::template_arguments_of(list) | std::views::transform(std::meta::dealias) |
                   std::ranges::to<std::vector>();
        }

        /** @brief Append unique dealiased element reflections from @p list to @p out. */
        consteval void AppendTypeListUnique(std::vector<std::meta::info>& out, std::meta::info list) {
            for (const auto type : TypeListTypesOf(list)) {
                if (std::find(out.begin(), out.end(), type) == out.end()) {
                    out.push_back(type);
                }
            }
        }

        /** @brief Append unique element reflections from the type-level @p List to @p out. */
        template<typename List>
        consteval void AppendTypeListUnique(std::vector<std::meta::info>& out) {
            AppendTypeListUnique(out, std::meta::dealias(^^List));
        }

        /** @brief Return a reflected list assembled from @p lists in order. */
        [[nodiscard]] consteval std::meta::info ConcatTypeListsInfo(std::vector<std::meta::info> lists) {
            std::vector<std::meta::info> result;
            for (const auto list : lists) {
                result.append_range(TypeListTypesOf(list));
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Return a reflected list with @p type prepended to @p list. */
        [[nodiscard]] consteval std::meta::info PushFrontInfo(std::meta::info type, std::meta::info list) {
            auto result = TypeListTypesOf(list);
            result.insert(result.begin(), type);
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Return a reflected list with @p type appended to @p list. */
        [[nodiscard]] consteval std::meta::info PushBackInfo(std::meta::info list, std::meta::info type) {
            auto result = TypeListTypesOf(list);
            result.push_back(type);
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Return @p list in reverse order. */
        [[nodiscard]] consteval std::meta::info ReverseTypeListInfo(std::meta::info list) {
            auto result = TypeListTypesOf(list);
            std::reverse(result.begin(), result.end());
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Return @p list without its first element. */
        [[nodiscard]] consteval std::meta::info TailInfo(std::meta::info list) {
            auto result = TypeListTypesOf(list);
            if (!result.empty()) {
                result.erase(result.begin());
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Remove the final element from @p list; an empty list remains empty. */
        [[nodiscard]] consteval std::meta::info PopBackInfo(std::meta::info list) {
            auto result = TypeListTypesOf(list);
            if (!result.empty()) {
                result.pop_back();
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Remove duplicate types while retaining their first occurrence. */
        [[nodiscard]] consteval std::meta::info UniqueTypeListInfo(std::meta::info list) {
            std::vector<std::meta::info> result;
            for (const auto type : TypeListTypesOf(list)) {
                if (std::find(result.begin(), result.end(), type) == result.end()) {
                    result.push_back(type);
                }
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Apply unary template @p Function to each element of @p list. */
        template<template<typename> typename Function, typename List>
        [[nodiscard]] consteval std::meta::info MapInfo() {
            std::vector<std::meta::info> result;
            constexpr auto types = std::define_static_array(std::meta::template_arguments_of(^^List));
            template for (constexpr std::meta::info type : types) {
                result.push_back(std::meta::substitute(^^Function, {type}));
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Retain elements of @p list satisfying reflected @p predicate. */
        [[nodiscard]] consteval std::meta::info FilterInfo(
            std::meta::info predicate, std::meta::info list) {
            std::vector<std::meta::info> result;
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (std::meta::extract<bool>(accepted)) {
                    result.push_back(type);
                }
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Instantiate variadic template @p Function with all elements of @p list. */
        template<template<typename...> typename Function, typename List>
        [[nodiscard]] consteval std::meta::info ApplyInfo() {
            return std::meta::substitute(^^Function, std::meta::template_arguments_of(^^List));
        }

        /** @brief Left-fold binary template @p Operation over @p list. */
        template<template<typename, typename> typename Operation, typename Initial, typename List>
        [[nodiscard]] consteval std::meta::info FoldInfo() {
            const auto list = std::meta::dealias(^^List);
            auto initial = std::meta::dealias(^^Initial);
            auto accumulator = initial;
            for (const auto type : TypeListTypesOf(list)) {
                accumulator = std::meta::substitute(^^Operation, {accumulator, type});
            }
            return accumulator;
        }

        /** @brief Return the first element satisfying reflected @p predicate, or `void`. */
        [[nodiscard]] consteval std::meta::info FindFirstInfo(
            std::meta::info predicate, std::meta::info list) {
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (std::meta::extract<bool>(accepted)) {
                    return type;
                }
            }
            return ^^void;
        }

        /** @brief Return the last element satisfying reflected @p predicate, or `void`. */
        [[nodiscard]] consteval std::meta::info FindLastInfo(
            std::meta::info predicate, std::meta::info list) {
            auto result = ^^void;
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (std::meta::extract<bool>(accepted)) {
                    result = type;
                }
            }
            return result;
        }

        /** @brief Return a reflected list of non-static data-member types of @p type. */
        template<typename T>
        [[nodiscard]] consteval std::meta::info DataMemberTypesInfo() {
            std::vector<std::meta::info> result;
            result.append_range(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()) |
                                std::views::transform(std::meta::type_of));
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Return reflected bool indicating whether all elements satisfy @p predicate. */
        [[nodiscard]] consteval std::meta::info AllOfInfo(
            std::meta::info predicate, std::meta::info list) {
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (!std::meta::extract<bool>(accepted)) {
                    return std::meta::reflect_constant(false);
                }
            }
            return std::meta::reflect_constant(true);
        }

        /** @brief Return reflected bool indicating whether any element satisfies @p predicate. */
        [[nodiscard]] consteval std::meta::info AnyOfInfo(
            std::meta::info predicate, std::meta::info list) {
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (std::meta::extract<bool>(accepted)) {
                    return std::meta::reflect_constant(true);
                }
            }
            return std::meta::reflect_constant(false);
        }

        /** @brief Return reflected bool indicating whether no element satisfies @p predicate. */
        [[nodiscard]] consteval std::meta::info NoneOfInfo(
            std::meta::info predicate, std::meta::info list) {
            const auto result = AnyOfInfo(predicate, list);
            if (IsMetaError(result)) {
                return result;
            }
            return std::meta::reflect_constant(!std::meta::extract<bool>(result));
        }

        /** @brief Return reflected count of elements satisfying @p predicate. */
        [[nodiscard]] consteval std::meta::info CountIfInfo(
            std::meta::info predicate, std::meta::info list) {
            std::size_t count = 0;
            for (const auto type : TypeListTypesOf(list)) {
                const auto accepted = InvokePredicate(predicate, type);
                if (IsMetaError(accepted)) {
                    return accepted;
                }
                if (std::meta::extract<bool>(accepted)) {
                    ++count;
                }
            }
            return std::meta::reflect_constant(count);
        }

        /** @brief Extract a reflected size, diagnosing an invalid meta computation. */
        [[nodiscard]] consteval std::size_t RequireSize(std::meta::info result) {
            if (IsMetaError(result) || !std::meta::is_value(result) ||
                !std::meta::is_integral_type(std::meta::type_of(result))) {
                throw std::define_static_string("Meta::RequireSize requires a reflected size result.");
            }
            return std::meta::extract<std::size_t>(result);
        }

    } // namespace Meta

    namespace Concept {

        /** @brief True when @p T is a `Traits::TypeList` specialization. */
        template<typename T>
        concept TypeList = Meta::IsTypeList(std::meta::dealias(^^std::remove_cvref_t<T>));

    } // namespace Concept

    namespace Traits {

        /** @brief Whether @p L is a `TypeList`. */
        template<typename L>
        inline constexpr bool IsTypeList = Concept::TypeList<L>;

        /** @brief Number of elements in @p L. */
        template<typename L>
            requires Concept::TypeList<L>
        inline constexpr std::size_t Length = std::remove_cvref_t<L>::size;

        /** @brief Return the @p I-th element of @p L. */
        template<typename L, std::size_t I>
            requires(Concept::TypeList<L> && I < Length<L>)
        using At = typename std::remove_cvref_t<L>::template At<I>;

        /** @brief First element of @p L; requires a non-empty list. */
        template<typename L>
            requires(Concept::TypeList<L> && Length<L> > 0)
        using Head = At<L, 0>;

        /** @brief Last element of @p L; requires a non-empty list. */
        template<typename L>
            requires(Concept::TypeList<L> && Length<L> > 0)
        using Last = At<L, Length<L> - 1>;

        /** @brief All but the first element of @p L. */
        template<typename L>
            requires Concept::TypeList<L>
        using Tail = Meta::InfoType<Meta::TailInfo(^^L)>;

        /** @brief Concatenate any number of type lists. */
        template<typename... Lists>
            requires(Concept::TypeList<Lists> && ...)
        using Concat = Meta::InfoType<Meta::ConcatTypeListsInfo({^^Lists...})>;

        /** @brief Apply unary template @p Function to each element. */
        template<template<typename> typename Function, typename List>
            requires Concept::TypeList<List>
        using MapT = Meta::InfoType<Meta::MapInfo<Function, List>()>;

        /** @brief Retain elements satisfying reflected @p Predicate. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        using FilterT = Meta::InfoType<Meta::FilterInfo(Predicate, ^^List)>;

        /** @brief Instantiate variadic template @p Function with all list elements. */
        template<template<typename...> typename Function, typename List>
            requires Concept::TypeList<List>
        using ApplyT = Meta::InfoType<Meta::ApplyInfo<Function, List>()>;

        /** @brief Left fold using binary template @p Operation and initial @p Initial. */
        template<template<typename, typename> typename Operation, typename Initial, typename List>
            requires Concept::TypeList<List>
        using FoldT = Meta::InfoType<Meta::FoldInfo<Operation, Initial, List>()>;

        /** @brief Index of @p T in @p List, or `size_t(-1)`. */
        template<typename List, typename T>
            requires Concept::TypeList<List>
        inline constexpr std::size_t IndexOf = std::remove_cvref_t<List>::template IndexOf<T>();

        /** @brief Whether @p List contains @p T. */
        template<typename List, typename T>
            requires Concept::TypeList<List>
        inline constexpr bool Contains = std::remove_cvref_t<List>::template Contains<T>();

        /** @brief First element satisfying reflected @p Predicate, or `void`. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        using FindFirstIf = Meta::InfoType<Meta::FindFirstInfo(Predicate, ^^List)>;

        /** @brief Last element satisfying reflected @p Predicate, or `void`. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        using FindLastIf = Meta::InfoType<Meta::FindLastInfo(Predicate, ^^List)>;

        /** @brief Prepend @p T to @p List. */
        template<typename T, typename List>
            requires Concept::TypeList<List>
        using PushFront = Meta::InfoType<Meta::PushFrontInfo(^^T, ^^List)>;

        /** @brief Append @p T to @p List. */
        template<typename List, typename T>
            requires Concept::TypeList<List>
        using PushBack = Meta::InfoType<Meta::PushBackInfo(^^List, ^^T)>;

        /** @brief Remove the final element from @p List. */
        template<typename List>
            requires Concept::TypeList<List>
        using PopBack = Meta::InfoType<Meta::PopBackInfo(^^List)>;

        /** @brief Reverse @p List. */
        template<typename List>
            requires Concept::TypeList<List>
        using Reverse = Meta::InfoType<Meta::ReverseTypeListInfo(^^List)>;

        /** @brief Remove duplicate types from @p List, retaining first occurrences. */
        template<typename List>
            requires Concept::TypeList<List>
        using Unique = Meta::InfoType<Meta::UniqueTypeListInfo(^^List)>;

        /** @brief Whether every element satisfies reflected @p Predicate. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        inline constexpr bool AllOf = Meta::RequirePredicateResult(Meta::AllOfInfo(Predicate, ^^List));

        /** @brief Whether any element satisfies reflected @p Predicate. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        inline constexpr bool AnyOf = Meta::RequirePredicateResult(Meta::AnyOfInfo(Predicate, ^^List));

        /** @brief Whether no element satisfies reflected @p Predicate. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        inline constexpr bool NoneOf = Meta::RequirePredicateResult(Meta::NoneOfInfo(Predicate, ^^List));

        /** @brief Count elements satisfying reflected @p Predicate. */
        template<std::meta::info Predicate, typename List>
            requires Concept::TypeList<List>
        inline constexpr std::size_t CountIf = Meta::RequireSize(Meta::CountIfInfo(Predicate, ^^List));

        /** @brief A `TypeList` of non-static data-member types of @p T. */
        template<typename T>
        using ToTypeList = Meta::InfoType<Meta::DataMemberTypesInfo<T>()>;

        template<typename... Ts>
        template<typename T>
        consteval std::size_t TypeList<Ts...>::IndexOf() noexcept {
            const auto types = Meta::TypeListTypesOf(^^TypeList);
            for (std::size_t index = 0; index < types.size(); ++index) {
                if (types[index] == std::meta::dealias(^^T)) {
                    return index;
                }
            }
            return static_cast<std::size_t>(-1);
        }

        template<typename... Ts>
        template<typename T>
        consteval bool TypeList<Ts...>::Contains() noexcept {
            return IndexOf<T>() != npos;
        }

        template<typename... Ts>
        template<typename T>
        consteval std::size_t TypeList<Ts...>::Count() noexcept {
            const auto target = std::meta::dealias(^^T);
            std::size_t count = 0;
            for (const auto type : Meta::TypeListTypesOf(^^TypeList)) {
                count += type == target;
            }
            return count;
        }

        template<typename... Ts>
        consteval bool TypeList<Ts...>::IsUnique() noexcept {
            return ((Count<Ts>() == 1) && ...);
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::Concat() const noexcept {
            return TypeList<Ts..., std::remove_cvref_t<Us>...>{};
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::Prepend() const noexcept {
            return TypeList<std::remove_cvref_t<Us>..., Ts...>{};
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::PrependUnique() const noexcept {
            return Meta::InfoType<Meta::UniqueTypeListInfo(^^TypeList<std::remove_cvref_t<Us>..., Ts...>)>();
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::AppendUnique() const noexcept {
            return Meta::InfoType<Meta::UniqueTypeListInfo(^^TypeList<Ts..., std::remove_cvref_t<Us>...>)>();
        }

        template<typename... Ts>
        template<typename... Us>
        consteval auto TypeList<Ts...>::Append() const noexcept {
            return TypeList<Ts..., std::remove_cvref_t<Us>...>{};
        }

        template<typename... Ts>
        consteval auto TypeList<Ts...>::Clear() const noexcept {
            return TypeList<>{};
        }

        template<typename... Ts>
        consteval auto TypeList<Ts...>::PopFront() const noexcept {
            return Meta::InfoType<Meta::TailInfo(^^TypeList)>{};
        }

        template<typename... Ts>
        consteval auto TypeList<Ts...>::PopBack() const noexcept {
            return Meta::InfoType<Meta::PopBackInfo(^^TypeList)>{};
        }

        template<typename... Ts>
        consteval auto TypeList<Ts...>::Reverse() const noexcept {
            return Meta::InfoType<Meta::ReverseTypeListInfo(^^TypeList)>{};
        }

        template<typename... Ts>
        consteval auto TypeList<Ts...>::Unique() const noexcept {
            return Meta::InfoType<Meta::UniqueTypeListInfo(^^TypeList)>{};
        }

    } // namespace Traits

} // namespace Sora
