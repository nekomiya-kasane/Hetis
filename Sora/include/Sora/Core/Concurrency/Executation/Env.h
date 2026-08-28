#pragma once

/**
 * @file Env.h
 * @brief Queryable execution environments and environment-provider customization points.
 * @ingroup Concurrency
 */

#include <concepts>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency::Exec {

    namespace Concept {

        /**
         * @brief Whether a type exposes a non-throwing const environment provider.
         * @tparam T Candidate provider type.
         * @par Example
         * @code{.cpp}
         * struct Receiver { constexpr int GetEnv() const noexcept { return 7; } };
         * static_assert(Sora::Exec::Concept::HasGetEnv<Receiver>);
         * static_assert(Sora::Exec::GetEnv(Receiver{}) == 7); // input Receiver{} -> output 7
         * @endcode
         */
        template<typename T>
        concept HasGetEnv = requires(const std::remove_reference_t<T>& object) {
            { object.GetEnv() } noexcept;
        };

    } // namespace Concept

    namespace Traits {

        /** @brief Result type returned by @ref GetEnv for @p T. */
        template<Concept::HasGetEnv T>
        using GetEnvResultType = decltype(std::declval<const std::remove_reference_t<T>&>().GetEnv());

    } // namespace Traits

    /**
     * @brief Obtain the execution environment supplied by an object.
     * @details The provider is invoked as a const lvalue and must be non-throwing. This mirrors the lifetime-neutral
     * environment access used by sender/receiver protocols while retaining Sora's PascalCase customization spelling.
     * @tparam T Provider type.
     * @param[in] object Object that owns or exposes the environment.
     * @return The environment value or view returned by `object.GetEnv()`.
     * @par Example
     * @code{.cpp}
     * struct Sender { constexpr int GetEnv() const noexcept { return 42; } };
     * constexpr Sender sender;
     * constexpr int result = Sora::Exec::GetEnv(sender); // input sender -> output 42
     * static_assert(result == 42);
     * @endcode
     */
    struct GetEnvTag {
        template<Concept::HasGetEnv T>
        using ResultType = Traits::GetEnvResultType<T>;

        template<Concept::HasGetEnv T>
        [[nodiscard]] constexpr auto operator()(const T& object) const noexcept -> ResultType<T> {
            return object.GetEnv();
        }
    };

    inline constexpr GetEnvTag GetEnv{};

    namespace Concept {

        /**
         * @brief Whether @p T can provide an execution environment through @ref GetEnv.
         * @tparam T Candidate provider type.
         * @par Example
         * @code{.cpp}
         * struct Sender { constexpr int GetEnv() const noexcept { return 1; } };
         * static_assert(Sora::Exec::Concept::EnvProvider<Sender>);
         * @endcode
         */
        template<typename T>
        concept EnvProvider = HasGetEnv<T>;

        /**
         * @brief Whether a query tag is an empty, trivially copyable value object.
         * @tparam T Candidate query tag.
         * @par Example
         * @code{.cpp}
         * struct GetName {};
         * static_assert(Sora::Exec::Concept::QueryableTag<GetName>); // input tag -> output true
         * @endcode
         */
        template<typename T>
        concept QueryableTag = std::is_empty_v<std::remove_cvref_t<T>> &&
                               std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

        /**
         * @brief Whether an environment satisfies the basic queryable-object contract.
         * @details P2300 intentionally keeps this contract independent of any particular query. Individual query
         * support is expressed by @ref QueryableFor.
         * @tparam T Candidate environment type.
         * @par Example
         * @code{.cpp}
         * struct Environment {};
         * static_assert(Sora::Exec::Concept::Queryable<Environment>); // input Environment -> output true
         * @endcode
         */
        template<typename T>
        concept Queryable = std::destructible<std::remove_cvref_t<T>>;

        /**
         * @brief Whether @p Env responds to @p QueriedTag with the Sora `Query` protocol.
         * @tparam Env Environment type.
         * @tparam QueriedTag Empty query tag type.
         * @tparam Args Additional query arguments.
         * @par Example
         * @code{.cpp}
         * struct GetName {};
         * struct Environment { constexpr int Query(GetName) const noexcept { return 3; } };
         * static_assert(Sora::Exec::Concept::QueryableFor<Environment, GetName>);
         * static_assert(Environment{}.Query(GetName{}) == 3); // input tag -> output 3
         * @endcode
         */
        template<typename Env, typename QueriedTag, typename... Args>
        concept QueryableFor = Queryable<Env> && QueryableTag<QueriedTag> &&
                               requires(const std::remove_reference_t<Env>& env,
                                        const std::remove_cvref_t<QueriedTag>& tag, Args&&... args) {
                                   env.Query(tag, std::forward<Args>(args)...);
                               };

        /** @brief Whether @ref QueryableFor is satisfied and the query is non-throwing. */
        template<typename Env, typename QueriedTag, typename... Args>
        concept NothrowQueryableFor = QueryableFor<Env, QueriedTag, Args...> &&
                                      requires(const std::remove_reference_t<Env>& env,
                                               const std::remove_cvref_t<QueriedTag>& tag, Args&&... args) {
                                          { env.Query(tag, std::forward<Args>(args)...) } noexcept;
                                      };

        /**
         * @brief Whether a query tag supplies a compile-time query implementation for an environment.
         * @details The static form is optional and is detected only when the tag exposes `Query<Env>(...)`.
         * @tparam Env Environment type.
         * @tparam QueriedTag Query tag type.
         * @tparam Args Additional query arguments.
         * @par Example
         * @code{.cpp}
         * struct Scheduler {};
         * struct GetScheduler {
         *     template<typename> static constexpr int Query(const Scheduler&) noexcept { return 9; }
         * };
         * static_assert(Sora::Exec::Concept::StaticallyQueryableFor<Scheduler, GetScheduler>);
         * static_assert(GetScheduler::Query<Scheduler>(Scheduler{}) == 9); // input Scheduler -> output 9
         * @endcode
         */
        template<typename Env, typename QueriedTag, typename... Args>
        concept StaticallyQueryableFor = QueryableTag<QueriedTag> && requires(const Env& env, Args&&... args) {
            { QueriedTag::template Query<Env>(env, std::forward<Args>(args)...) };
        };

        /** @brief Whether a query tag provides a compile-time validation hook for an environment. */
        template<typename T, typename Env, typename... Args>
        concept HasValidation = requires {
            { T::template Validate<Env, Args...>() } -> std::convertible_to<bool>;
        };

    } // namespace Concept

    namespace Traits {

        /** @brief Result type of a valid Sora environment query. */
        template<typename Env, typename QueriedTag, typename... Args>
        requires Concept::QueryableFor<Env, QueriedTag, Args...>
        using QueryableResultType = decltype(std::declval<const std::remove_reference_t<Env>&>().Query(
            std::declval<const std::remove_cvref_t<QueriedTag>&>(), std::declval<Args>()...));

    } // namespace Traits

    namespace Detail {

        template<bool QuerySupported, typename Env, typename Tag, typename Default, typename... Args>
        struct QueryReturn;

        template<typename Env, typename Tag, typename Default, typename... Args>
        struct QueryReturn<true, Env, Tag, Default, Args...> {
            using Type = Traits::QueryableResultType<Env, Tag, Args...>;
        };

        template<typename Env, typename Tag, typename Default, typename... Args>
        struct QueryReturn<false, Env, Tag, Default, Args...> {
            using Type = Default;
        };

        template<bool QuerySupported, typename Env, typename Tag, typename Default, typename... Args>
        using QueryReturnType = typename QueryReturn<QuerySupported, Env, Tag, Default, Args...>::Type;

    } // namespace Detail

    /**
     * @brief Reusable query CPO base with a compile-time fallback.
     * @tparam Tag Default query tag.
     * @tparam Default Fallback value returned when the environment does not answer the query.
     * @par Example
     * @code{.cpp}
     * struct GetPriority {};
     * struct Environment { constexpr int Query(GetPriority) const noexcept { return 8; } };
     * inline constexpr QueryBase<GetPriority, 0> priority{};
     * static_assert(priority(Environment{}) == 8); // input Environment -> output 8
     * @endcode
     */
    template<Concept::QueryableTag Tag, auto Default = std::ignore>
    struct QueryBase {
        template<Concept::QueryableTag InnerTag = Tag, typename Env, typename... Args>
        requires Concept::QueryableFor<Env, InnerTag, Args...> || std::copy_constructible<decltype(Default)>
        [[nodiscard]] constexpr auto operator()(const Env& env, Args&&... args) const
            noexcept(Concept::QueryableFor<Env, InnerTag, Args...>
                         ? Concept::NothrowQueryableFor<Env, InnerTag, Args...>
                         : std::is_nothrow_copy_constructible_v<decltype(Default)>)
            -> Detail::QueryReturnType<Concept::QueryableFor<Env, InnerTag, Args...>, Env, InnerTag, decltype(Default),
                                       Args...> {
            if constexpr (Concept::QueryableFor<Env, InnerTag, Args...>) {
                return env.Query(InnerTag{}, std::forward<Args>(args)...);
            } else {
                return Default;
            }
        }
    };

    /**
     * @brief Query tag used to opt an environment property into forwarding.
     * @par Example
     * @code{.cpp}
     * struct GetScheduler : Sora::Exec::ForwardingQueryTag {};
     * static_assert(Sora::Exec::ForwardingQuery(GetScheduler{})); // input tag -> output true
     * @endcode
     */
    struct ForwardingQueryTag {
        template<typename QueryTag>
        consteval bool operator()(QueryTag) const noexcept {
            if constexpr (Concept::QueryableFor<QueryTag, ForwardingQueryTag>) {
                return static_cast<bool>(QueryTag{}.Query(ForwardingQueryTag{}));
            } else {
                return std::derived_from<QueryTag, ForwardingQueryTag>;
            }
        }
    };

    inline constexpr ForwardingQueryTag ForwardingQuery{};

    namespace Concept {

        /** @brief Whether @p T is a query tag accepted by @ref ForwardingQuery. */
        template<typename T>
        concept ForwardingQueryableTag = QueryableTag<T> && requires {
            { ForwardingQuery(T{}) } -> std::same_as<bool>;
        };

    } // namespace Concept

    /**
     * @brief Invoke a callable query or return a supplied fallback value.
     * @details This adapter deliberately invokes @p queryer directly; it is not a replacement for stdexec's
     * `query` CPO. The fallback is decayed so an lvalue default produces an independent value.
     * @tparam Queryer Callable query object.
     * @tparam Env Input environment.
     * @tparam Default Fallback value type.
     * @tparam Args Additional query arguments.
     * @par Example
     * @code{.cpp}
     * struct Double { constexpr int operator()(int value) const noexcept { return value * 2; } };
     * struct Unsupported {};
     * static_assert(Sora::Exec::QueryOr(Double{}, 6, 0) == 12);       // input 6 -> output 12
     * static_assert(Sora::Exec::QueryOr(Unsupported{}, 6, 0) == 0);  // unsupported input -> fallback 0
     * @endcode
     */
    struct QueryOrTag {
        template<typename Queryer, typename Env, typename Default, typename... Args>
        requires std::invocable<Queryer, Env, Args...>
        [[nodiscard]] constexpr decltype(auto) operator()(Queryer&& queryer, Env&& env, Default&&,
                                                          Args&&... args) const
            noexcept(std::is_nothrow_invocable_v<Queryer, Env, Args...>) {
            return std::invoke(std::forward<Queryer>(queryer), std::forward<Env>(env), std::forward<Args>(args)...);
        }

        template<typename Queryer, typename Env, typename Default, typename... Args>
        requires(!std::invocable<Queryer, Env, Args...>)
        [[nodiscard]] constexpr auto operator()(Queryer&&, Env&&, Default&& defaultValue, Args&&...) const
            noexcept(std::is_nothrow_constructible_v<std::decay_t<Default>, Default&&>) -> std::decay_t<Default> {
            return std::forward<Default>(defaultValue);
        }
    };

    inline constexpr QueryOrTag QueryOr{};

    namespace Detail {

        template<bool Invocable, typename Queryer, typename Env, typename Default, typename... Args>
        struct QueryOrResult;

        template<typename Queryer, typename Env, typename Default, typename... Args>
        struct QueryOrResult<true, Queryer, Env, Default, Args...> {
            using Type = std::invoke_result_t<Queryer, Env, Args...>;
        };

        template<typename Queryer, typename Env, typename Default, typename... Args>
        struct QueryOrResult<false, Queryer, Env, Default, Args...> {
            using Type = std::decay_t<Default>;
        };

        template<bool Invocable, typename Queryer, typename Env, typename Default, typename... Args>
        using QueryOrResultType = typename QueryOrResult<Invocable, Queryer, Env, Default, Args...>::Type;

    } // namespace Detail

    namespace Traits {

        /** @brief Decayed result type selected by @ref QueryOr. */
        template<typename Queryer, typename Env, typename Default, typename... Args>
        using QueryOrResultType = Detail::QueryOrResultType<std::invocable<Queryer, Env, Args...>, Queryer, Env,
                                                           Default, Args...>;

    } // namespace Traits

} // namespace Sora::Concurrency::Exec

namespace Sora {

    namespace Exec = Concurrency::Exec;

    namespace Concept {

        inline namespace Exec {

            template<typename T>
            concept Queryable = Sora::Exec::Concept::Queryable<T>;

            template<typename Env, typename QueriedTag, typename... Args>
            concept QueryableFor = Sora::Exec::Concept::QueryableFor<Env, QueriedTag, Args...>;

            template<typename Env, typename QueriedTag, typename... Args>
            concept NothrowQueryableFor = Sora::Exec::Concept::NothrowQueryableFor<Env, QueriedTag, Args...>;

        } // namespace Exec

        inline namespace Concurrency {

            namespace Exec = Sora::Concurrency::Exec::Concept;

        } // namespace Concurrency

    } // namespace Concept

} // namespace Sora
