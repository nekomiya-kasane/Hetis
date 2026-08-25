/**
 * @file MetaInvocable.h
 * @brief C++26 reflection-native template application and higher-order algorithms.
 * @ingroup Core
 *
 * @details A meta-function is a `std::meta::info`; application is
 * `std::meta::substitute`. The algorithms below are the Sora semantic layer
 * over the useful, domain-independent part of stdexec's `__meta.hpp`.
 */
#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <meta>
#include <ranges>
#include <vector>

#include <Sora/Core/Traits/MetaError.h>
#include <Sora/Core/Traits/MetaInfo.h>
#include <Sora/Core/Traits/MetaPredicate.h>
#include <Sora/Core/Traits/TypeList.h>

namespace Sora {

    namespace Meta {

        /**
         * @brief True when @p Function accepts exactly the reflected @p Arguments.
         * @tparam Function Template, alias template, or member template reflection.
         * @tparam Arguments Type, value, or declaration reflections supplied to the template.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept DirectlyInvocable = std::meta::is_template(Function) &&
                                    std::meta::can_substitute(Function, {Arguments...});

        /**
         * @brief Apply a reflected template and propagate its first error input.
         *
         * @details
         * [
         * I(F,A)=
         * \begin{cases} E_i & E_i\text{ is the first error in }(F,A),\\
         * \operatorname{substitute}(F,A) & \operatorname{can\_substitute}(F,A),\\
         * \operatorname{MetaError}(\mathrm{NonInvocable},F,A) & \text{otherwise.}
         * \end{cases}
         * ]
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info Invoke = [] consteval {
            constexpr auto error = FirstMetaErrorInfo<Function, Arguments...>();
            if constexpr (error != std::meta::info{}) {
                return error;
            } else if constexpr (DirectlyInvocable<Function, Arguments...>) {
                return std::meta::substitute(Function, {Arguments...});
            } else {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, Arguments...});
            }
        }();

        /**
         * @brief True when applying @p Function does not produce a meta error.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept Invocable = !IsMetaError(Invoke<Function, Arguments...>);

        template<std::meta::info Function, typename List>
        consteval std::meta::info ApplyToTypeListOf();

        namespace Detail {

            /** @brief Convert a successful result to a type or a typed-result error. */
            template<std::meta::info Result, std::meta::info Function, std::meta::info... Arguments>
            [[nodiscard]] consteval std::meta::info RequireType() {
                if constexpr (IsMetaError(Result) || std::meta::is_type(Result)) {
                    return Result;
                } else {
                    return MakeMetaError(^^Error::NonTypeResultTag, {Function, Arguments...});
                }
            }

            template<std::meta::info Function, std::meta::info... Front>
            struct BindFrontClosure {
                template<typename... Arguments>
                using Apply = InfoType<Invoke<Function, Front..., ^^Arguments...>>;
            };

            template<std::meta::info Function, std::meta::info... Back>
            struct BindBackClosure {
                template<typename... Arguments>
                using Apply = InfoType<Invoke<Function, ^^Arguments..., Back...>>;
            };

            template<std::meta::info Outer, std::meta::info Inner>
            struct ComposeClosure {
                template<typename... Arguments>
                using Apply = InfoType<Invoke<Outer, Invoke<Inner, ^^Arguments...>>>;
            };

            template<std::meta::info Function, std::meta::info Continuation>
            struct TransformClosure {
                template<typename... Arguments>
                using Apply = InfoType<Invoke<Continuation, Invoke<Function, ^^Arguments>...>>;
            };

            template<std::meta::info Result>
            struct ConstantClosure {
                template<typename...>
                using Apply = InfoType<Result>;
            };

            template<std::meta::info Function>
            struct UncurryClosure {
                template<typename List>
                using Apply = InfoType<ApplyToTypeListOf<Function, List>()>;
            };

        } // namespace Detail

        /**
         * @brief Type projection of @ref Invoke; a non-type result becomes `NonTypeResultTag`.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        using InvokeType = InfoType<Detail::RequireType<Invoke<Function, Arguments...>, Function, Arguments...>()>;

        /**
         * @brief Invoke a reflected template with ordinary C++ type arguments.
         * @code{.cpp}
         * using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
         * static_assert(std::same_as<Pair, std::pair<int, double>>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        using InvokeResult = InvokeType<Function, ^^Arguments...>;

        /**
         * @brief True when the invocation succeeds and produces a type entity.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Ordinary C++ types reflected for application.
         */
        template<std::meta::info Function, typename... Arguments>
        concept TypeInvocable = Invocable<Function, ^^Arguments...> &&
                                std::meta::is_type(Invoke<Function, ^^Arguments...>);

        /** @brief Select @p Then or @p Else using the standard conditional template. */
        template<bool Condition, typename Then, typename Else>
        using If = InvokeType<^^std::conditional_t, std::meta::reflect_constant(Condition), ^^Then, ^^Else>;

        /** @brief Invoke @p Function or return @p Default when application fails. */
        template<std::meta::info Default, std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info InvokeOr = [] consteval {
            constexpr auto result = Invoke<Function, Arguments...>;
            return IsMetaError(result) ? Default : result;
        }();

        /**
         * @brief Return a reflected function with leading arguments bound.
         * @tparam Function Reflected template to invoke after binding.
         * @tparam Front Reflections inserted before call-site arguments.
         */
        template<std::meta::info Function, std::meta::info... Front>
        inline constexpr std::meta::info BindFront = ^^Detail::BindFrontClosure<Function, Front...>::template Apply;

        /**
         * @brief Return a reflected function with trailing arguments bound.
         * @tparam Function Reflected template to invoke after binding.
         * @tparam Back Reflections inserted after call-site arguments.
         */
        template<std::meta::info Function, std::meta::info... Back>
        inline constexpr std::meta::info BindBack = ^^Detail::BindBackClosure<Function, Back...>::template Apply;

        /**
         * @brief Compose `Outer(Inner(args...))` as a reflected function.
         * @tparam Outer Outer reflected template.
         * @tparam Inner Inner reflected template.
         */
        template<std::meta::info Outer, std::meta::info Inner>
        inline constexpr std::meta::info Compose = ^^Detail::ComposeClosure<Outer, Inner>::template Apply;

        /**
         * @brief Transform each argument and pass the results to @p Continuation.
         * @tparam Function Unary reflected template applied pointwise.
         * @tparam Continuation Reflected variadic template receiving transformed arguments.
         */
        template<std::meta::info Function, std::meta::info Continuation = ^^Traits::TypeList>
        inline constexpr std::meta::info Transform =
            ^^Detail::TransformClosure<Function, Continuation>::template Apply;

        /**
         * @brief Return a reflected function that always yields @p Result.
         * @tparam Result Reflected entity returned for every application.
         */
        template<std::meta::info Result>
        inline constexpr std::meta::info Constant = ^^Detail::ConstantClosure<Result>::template Apply;

        /** @brief Return a reflected function that applies @p Function to a TypeList argument. */
        template<std::meta::info Function>
        inline constexpr std::meta::info Uncurry = ^^Detail::UncurryClosure<Function>::template Apply;

        /** @brief Apply @p Function to all arguments in a Sora TypeList. */
        [[nodiscard]] consteval std::meta::info ApplyToTypeList(std::meta::info Function, std::meta::info List) {
            List = std::meta::dealias(List);
            if (!IsTypeList(List)) {
                throw std::define_static_string("Meta::ApplyToTypeList requires Sora::Traits::TypeList.");
            }
            auto arguments = std::meta::template_arguments_of(List);
            if (IsMetaError(Function)) {
                return Function;
            }
            for (const auto argument : arguments) {
                if (IsMetaError(argument)) {
                    return argument;
                }
            }
            if (!std::meta::is_template(Function) || !std::meta::can_substitute(Function, arguments)) {
                std::vector<std::meta::info> context{Function, List};
                return MakeMetaError(^^Error::NonInvocableTag, context);
            }
            return std::meta::substitute(Function, arguments);
        }

        /** @brief Replace every occurrence of @p Old in @p List with @p New. */
        [[nodiscard]] consteval std::meta::info ReplaceTypeList(
            std::meta::info Old, std::meta::info New, std::meta::info List) {
            auto result = TypeListTypesOf(List);
            for (auto& type : result) {
                if (type == Old) {
                    type = New;
                }
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Remove every occurrence of @p Needle from @p List. */
        [[nodiscard]] consteval std::meta::info RemoveFromTypeList(
            std::meta::info Needle, std::meta::info List) {
            auto result = TypeListTypesOf(List);
            std::erase_if(result, [Needle](const auto type) { return type == Needle; });
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /**
         * @brief Zip two TypeLists, invoke @p Function pairwise, and return a TypeList of results.
         * @pre Both lists have equal length and @p Function accepts two arguments.
         */
        [[nodiscard]] consteval std::meta::info ZipWith2ToTypeList(
            std::meta::info Function, std::meta::info Left, std::meta::info Right) {
            auto left = TypeListTypesOf(Left);
            auto right = TypeListTypesOf(Right);
            if (left.size() != right.size()) {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, Left, Right});
            }
            std::vector<std::meta::info> result;
            result.reserve(left.size());
            for (std::size_t index = 0; index < left.size(); ++index) {
                const std::array arguments{left[index], right[index]};
                if (!std::meta::is_template(Function) || !std::meta::can_substitute(Function, arguments)) {
                    return MakeMetaError(^^Error::NonInvocableTag, {Function, left[index], right[index]});
                }
                result.push_back(std::meta::substitute(Function, arguments));
                if (IsMetaError(result.back())) {
                    return result.back();
                }
            }
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Construct a TypeList containing @p Count copies of @p Value. */
        [[nodiscard]] consteval std::meta::info FillTypeList(std::size_t Count, std::meta::info Value) {
            std::vector<std::meta::info> result(Count, Value);
            return std::meta::substitute(^^Traits::TypeList, result);
        }

        /** @brief Apply @p Function to a TypeList from left to right. */
        [[nodiscard]] consteval std::meta::info FoldLeftToTypeList(
            std::meta::info Function, std::meta::info Initial, std::meta::info List) {
            List = std::meta::dealias(List);
            if (!IsTypeList(List)) {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, Initial, List});
            }
            if (IsMetaError(Function) || IsMetaError(Initial)) {
                return IsMetaError(Function) ? Function : Initial;
            }
            auto accumulator = Initial;
            for (const auto type : std::meta::template_arguments_of(List)) {
                if (IsMetaError(type)) {
                    return type;
                }
                const std::array arguments{accumulator, type};
                if (!std::meta::is_template(Function) || !std::meta::can_substitute(Function, arguments)) {
                    return MakeMetaError(^^Error::NonInvocableTag, {Function, accumulator, type});
                }
                accumulator = std::meta::substitute(Function, arguments);
                if (IsMetaError(accumulator)) {
                    return accumulator;
                }
            }
            return accumulator;
        }

        /** @brief Apply @p Function to a TypeList from right to left. */
        [[nodiscard]] consteval std::meta::info FoldRightToTypeList(
            std::meta::info Function, std::meta::info Initial, std::meta::info List) {
            List = std::meta::dealias(List);
            if (!IsTypeList(List)) {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, Initial, List});
            }
            if (IsMetaError(Function) || IsMetaError(Initial)) {
                return IsMetaError(Function) ? Function : Initial;
            }
            auto accumulator = Initial;
            auto types = std::meta::template_arguments_of(List);
            for (auto it = types.rbegin(); it != types.rend(); ++it) {
                if (IsMetaError(*it)) {
                    return *it;
                }
                const std::array arguments{*it, accumulator};
                if (!std::meta::is_template(Function) || !std::meta::can_substitute(Function, arguments)) {
                    return MakeMetaError(^^Error::NonInvocableTag, {Function, *it, accumulator});
                }
                accumulator = std::meta::substitute(Function, arguments);
                if (IsMetaError(accumulator)) {
                    return accumulator;
                }
            }
            return accumulator;
        }

        /** @brief Compile-time typed bridge for applying a reflected function to a Sora TypeList. */
        template<std::meta::info Function, typename List>
        [[nodiscard]] consteval std::meta::info ApplyToTypeListOf() {
            constexpr auto arguments = std::define_static_array(std::meta::template_arguments_of(^^List));
            if (IsMetaError(Function)) {
                return Function;
            }
            template for (constexpr std::meta::info argument : arguments) {
                if constexpr (IsMetaError(argument)) {
                    return argument;
                }
            }
            if constexpr (!std::meta::can_substitute(Function, arguments)) {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, ^^List});
            } else {
                return std::meta::substitute(Function, arguments);
            }
        }

        /** @brief Typed bridge for a left fold over @p List. */
        template<std::meta::info Function, typename Initial, typename List>
        [[nodiscard]] consteval std::meta::info FoldLeftToTypeListOf() {
            constexpr auto types = std::define_static_array(std::meta::template_arguments_of(^^List));
            if (IsMetaError(Function)) {
                return Function;
            }
            if (IsMetaError(^^Initial)) {
                return ^^Initial;
            }
            auto accumulator = ^^Initial;
            template for (constexpr std::meta::info type : types) {
                if constexpr (IsMetaError(type)) {
                    return type;
                }
                if (!std::meta::can_substitute(Function, {accumulator, type})) {
                    return MakeMetaError(^^Error::NonInvocableTag, {Function, accumulator, type});
                } else {
                    accumulator = std::meta::substitute(Function, {accumulator, type});
                }
            }
            return accumulator;
        }

        /** @brief Typed bridge for a right fold over @p List. */
        template<std::meta::info Function, typename Initial, typename List>
        [[nodiscard]] consteval std::meta::info FoldRightToTypeListOf() {
            constexpr auto types = std::define_static_array(std::meta::template_arguments_of(^^List));
            if (IsMetaError(Function)) {
                return Function;
            }
            if (IsMetaError(^^Initial)) {
                return ^^Initial;
            }
            auto accumulator = ^^Initial;
            for (auto it = types.rbegin(); it != types.rend(); ++it) {
                const auto type = *it;
                if (IsMetaError(type)) {
                    return type;
                }
                if (!std::meta::can_substitute(Function, {type, accumulator})) {
                    return MakeMetaError(^^Error::NonInvocableTag, {Function, type, accumulator});
                }
                accumulator = std::meta::substitute(Function, {type, accumulator});
            }
            return accumulator;
        }

        /**
         * @brief Return the number of reflected arguments passed to a variadic function.
         * @tparam Arguments Reflections counted by the variable template.
         */
        template<std::meta::info... Arguments>
        inline constexpr std::size_t Size = sizeof...(Arguments);

        /**
         * @brief Return whether @p Needle is equal to one of @p Arguments.
         * @tparam Needle Reflection being searched for.
         * @tparam Arguments Reflections searched in source order.
         */
        template<std::meta::info Needle, std::meta::info... Arguments>
        inline constexpr bool Contains = ((Needle == Arguments) || ...);

        /**
         * @brief Return the number of arguments equal to @p Needle.
         * @tparam Needle Reflection being counted.
         * @tparam Arguments Reflections searched in source order.
         */
        template<std::meta::info Needle, std::meta::info... Arguments>
        inline constexpr std::size_t Count = (std::size_t{0} + ... + (Needle == Arguments ? 1u : 0u));

        /** @brief Return whether every argument satisfies reflected unary @p Predicate. */
        template<std::meta::info Predicate, std::meta::info... Arguments>
        consteval bool AllOf() {
            return (EvaluatePredicate(Predicate, Arguments) && ...);
        }

        /** @brief Return whether at least one argument satisfies reflected unary @p Predicate. */
        template<std::meta::info Predicate, std::meta::info... Arguments>
        consteval bool AnyOf() {
            return (EvaluatePredicate(Predicate, Arguments) || ...);
        }

        /** @brief Return whether no argument satisfies reflected unary @p Predicate. */
        template<std::meta::info Predicate, std::meta::info... Arguments>
        consteval bool NoneOf() {
            return !AnyOf<Predicate, Arguments...>();
        }

        /** @brief Return the first argument equal to @p Needle, or an empty reflection. */
        template<std::meta::info Needle, std::meta::info... Arguments>
        consteval std::meta::info Find(std::meta::info fallback = {}) {
            for (const auto argument : std::array{Arguments...}) {
                if (argument == Needle) {
                    return argument;
                }
            }
            return fallback;
        }

        /** @brief Return the zero-based index of @p Needle, or `size_t(-1)` when absent. */
        template<std::meta::info Needle, std::meta::info... Arguments>
        consteval std::size_t IndexOf() noexcept {
            constexpr auto values = std::array{Arguments...};
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (values[index] == Needle) {
                    return index;
                }
            }
            return static_cast<std::size_t>(-1);
        }

        /**
         * @brief Return the first reflected argument, or an empty reflection for an empty pack.
         * @tparam Arguments Reflections searched in source order.
         */
        template<std::meta::info... Arguments>
        consteval std::meta::info Front() {
            if constexpr (sizeof...(Arguments) == 0) {
                return {};
            } else {
                return std::array{Arguments...}.front();
            }
        }

        /**
         * @brief Return the sole reflected argument, or an empty reflection otherwise.
         * @tparam Arguments Zero or one reflected argument.
         */
        template<std::meta::info... Arguments>
            requires(sizeof...(Arguments) <= 1)
        consteval std::meta::info Single() {
            return Front<Arguments...>();
        }

        /** @brief Return the sole reflected argument, or @p fallback when the pack is empty. */
        template<std::meta::info Fallback, std::meta::info... Arguments>
            requires(sizeof...(Arguments) <= 1)
        consteval std::meta::info SingleOr() {
            if constexpr (sizeof...(Arguments) == 0) {
                return Fallback;
            } else {
                return Front<Arguments...>();
            }
        }

        /**
         * @brief Return the @p Index-th reflected argument using C++26 pack indexing.
         * @tparam Index Zero-based argument index.
         * @tparam Arguments Reflections searched in source order.
         */
        template<std::size_t Index, std::meta::info... Arguments>
            requires(Index < sizeof...(Arguments))
        consteval std::meta::info At() {
            return std::array{Arguments...}[Index];
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Type-level application of @p Function to all members of @p List. */
        template<std::meta::info Function, typename List>
        using Apply = Meta::InfoType<Meta::ApplyToTypeListOf<Function, List>()>;

        /** @brief Type-level replacement in a TypeList. */
        template<typename Old, typename New, typename List>
        using Replace = Meta::InfoType<Meta::ReplaceTypeList(^^Old, ^^New, ^^List)>;

        /** @brief Type-level removal from a TypeList. */
        template<typename Needle, typename List>
        using Remove = Meta::InfoType<Meta::RemoveFromTypeList(^^Needle, ^^List)>;

        /** @brief Type-level pairwise zip and invocation. */
        template<std::meta::info Function, typename Left, typename Right>
        using ZipWith2 = Meta::InfoType<Meta::ZipWith2ToTypeList(Function, ^^Left, ^^Right)>;

        /** @brief Type-level left fold over @p List. */
        template<std::meta::info Function, typename Initial, typename List>
        using FoldLeft = Meta::InfoType<Meta::FoldLeftToTypeListOf<Function, Initial, List>()>;

        /** @brief Type-level right fold over @p List. */
        template<std::meta::info Function, typename Initial, typename List>
        using FoldRight = Meta::InfoType<Meta::FoldRightToTypeListOf<Function, Initial, List>()>;

    } // namespace Traits

    namespace Concept {

        /** @brief True when a reflected invocation produces a C++ type. */
        template<std::meta::info Function, typename... Arguments>
        concept MetaInvocable = Meta::TypeInvocable<Function, Arguments...>;

    } // namespace Concept

} // namespace Sora
