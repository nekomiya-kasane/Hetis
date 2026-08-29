#pragma once

/**
 * @file Env.h
 * @brief Queryable execution environments and environment-provider customization points.
 * @ingroup Concurrency
 *
 * @details This header defines Sora's private execution-environment protocol. `GetEnv` and `Query` intentionally use
 * PascalCase and are not adapters for `stdexec::get_env`, `stdexec::query`, or `forwarding_query`; identical-looking
 * names do not imply interoperability. Implementations, concepts, and result projections in this file must therefore
 * be reviewed against this protocol's own expression, forwarding, fallback, and `noexcept` contracts.
 */

#include <concepts>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency::Exec {

    namespace Detail {

        template<typename Env, typename QueriedTag, typename... Args>
        concept QueryExpression = requires(const std::remove_reference_t<Env>& env, Args&&... args) {
            env.Query(std::remove_cvref_t<QueriedTag>{}, std::forward<Args>(args)...);
        };

        template<typename QueriedTag, typename Env, typename... Args>
        requires QueryExpression<Env, QueriedTag, Args...>
        [[nodiscard]] constexpr decltype(auto) InvokeQuery(const std::remove_reference_t<Env>& env, Args&&... args)
            noexcept(noexcept(env.Query(std::remove_cvref_t<QueriedTag>{}, std::forward<Args>(args)...))) {
            return env.Query(std::remove_cvref_t<QueriedTag>{}, std::forward<Args>(args)...);
        }

    } // namespace Detail

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

        /**
         * @brief Result type returned by @ref GetEnv for @p T.
         * @tparam T Non-throwing const environment provider.
         * @par Example
         * @code{.cpp}
         * struct Provider { constexpr long GetEnv() const noexcept { return 5; } };
         * using Result = Sora::Exec::Traits::GetEnvResultType<Provider>;
         * static_assert(std::same_as<Result, long>); // input Provider -> output long
         * @endcode
         */
        template<Concept::HasGetEnv T>
        using GetEnvResultType = decltype(std::declval<const std::remove_reference_t<T>&>().GetEnv());

    } // namespace Traits

    /**
     * @brief Obtain the execution environment supplied by an object.
     * @details Sora's private provider protocol invokes @p object as a const lvalue and requires GetEnv() to be
     * non-throwing. No stdexec customization or interoperability is implied.
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
        /** @brief Result type produced for provider @p T. */
        template<Concept::HasGetEnv T>
        using ResultType = Traits::GetEnvResultType<T>;

        template<Concept::HasGetEnv T>
        [[nodiscard]] constexpr auto operator()(const T& object) const noexcept -> ResultType<T> {
            return object.GetEnv();
        }
    };

    /** @brief Sora-private environment-provider CPO. */
    inline constexpr GetEnvTag GetEnv{};

    namespace Concept {

        /**
         * @brief Whether @p T can provide an execution environment through @ref GetEnv.
         * @tparam T Candidate provider type.
         * @par Example
         * @code{.cpp}
         * struct Sender { constexpr int GetEnv() const noexcept { return 1; } };
         * static_assert(Sora::Exec::Concept::EnvProvider<Sender>); // input Sender -> output true
         * @endcode
         */
        template<typename T>
        concept EnvProvider = HasGetEnv<T>;

        /**
         * @brief Whether a query tag is a trivially default-constructible, stateless identity value.
         * @details Query tags carry only a compile-time operation identity. Runtime query state belongs in additional
         * arguments, so tag types must be empty, trivially copyable, and trivially constructible as Tag{}.
         * @tparam T Candidate query tag.
         * @par Example
         * @code{.cpp}
         * struct GetName {};
         * struct StatefulQuery { int channel; };
         * static_assert(Sora::Exec::Concept::QueryableTag<GetName>);        // input GetName -> output true
         * static_assert(!Sora::Exec::Concept::QueryableTag<StatefulQuery>); // input stateful tag -> output false
         * @endcode
         */
        template<typename T>
        concept QueryableTag = std::default_initializable<std::remove_cvref_t<T>> &&
                               std::is_trivially_default_constructible_v<std::remove_cvref_t<T>> &&
                               std::is_empty_v<std::remove_cvref_t<T>> &&
                               std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

        /**
         * @brief Whether an environment satisfies the basic queryable-object contract.
         * @details Sora keeps environment lifetime eligibility independent of individual query support, which is
         * expressed by @ref QueryableFor.
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
                               requires(const std::remove_reference_t<Env>& env, Args&&... args) {
                                   Detail::InvokeQuery<QueriedTag, Env, Args...>(env, std::forward<Args>(args)...);
                               };

        /**
         * @brief Whether a supported Sora query is non-throwing.
         * @tparam Env Environment type.
         * @tparam QueriedTag Stateless query identity.
         * @tparam Args Additional query argument types.
         * @par Example
         * @code{.cpp}
         * struct GetValue {};
         * struct StableEnv { constexpr int Query(GetValue) const noexcept { return 4; } };
         * struct ThrowingEnv { int Query(GetValue) const { return 4; } };
         * static_assert(Sora::Exec::Concept::NothrowQueryableFor<StableEnv, GetValue>);    // input -> true
         * static_assert(!Sora::Exec::Concept::NothrowQueryableFor<ThrowingEnv, GetValue>); // input -> false
         * @endcode
         */
        template<typename Env, typename QueriedTag, typename... Args>
        concept NothrowQueryableFor = QueryableFor<Env, QueriedTag, Args...> &&
                                      requires(const std::remove_reference_t<Env>& env, Args&&... args) {
                                          {
                                              Detail::InvokeQuery<QueriedTag, Env, Args...>(
                                                  env, std::forward<Args>(args)...)
                                          } noexcept;
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

        /**
         * @brief Whether a query tag provides a validation result for an environment and argument list.
         * @details This concept detects the independent Validate<Env, Args...>() hook and requires only a result
         * convertible to bool; it does not invoke the hook or require the result to be true.
         * @tparam T Candidate validation provider.
         * @tparam Env Environment type supplied to the validation template.
         * @tparam Args Additional validation argument types.
         * @par Example
         * @code{.cpp}
         * struct CheckEnvironment {
         *     template<typename Env, typename...> static consteval bool Validate() { return sizeof(Env) > 0; }
         * };
         * static_assert(Sora::Exec::Concept::HasValidation<CheckEnvironment, int>);
         * static_assert(CheckEnvironment::Validate<int>()); // input environment type int -> output true
         * @endcode
         */
        template<typename T, typename Env, typename... Args>
        concept HasValidation = requires {
            { T::template Validate<Env, Args...>() } -> std::convertible_to<bool>;
        };

    } // namespace Concept

    namespace Traits {

        /**
         * @brief Result type of the exact query expression used by @ref QueryBase.
         * @tparam Env Environment type.
         * @tparam QueriedTag Stateless query identity.
         * @tparam Args Additional query argument types.
         * @par Example
         * @code{.cpp}
         * struct GetValue {};
         * struct Environment { constexpr short Query(GetValue) const noexcept { return 6; } };
         * using Result = Sora::Exec::Traits::QueryableResultType<Environment, GetValue>;
         * static_assert(std::same_as<Result, short>); // input Environment and GetValue -> output short
         * @endcode
         */
        template<typename Env, typename QueriedTag, typename... Args>
        requires Concept::QueryableFor<Env, QueriedTag, Args...>
        using QueryableResultType = decltype(Detail::InvokeQuery<QueriedTag, Env, Args...>(
            std::declval<const std::remove_reference_t<Env>&>(), std::declval<Args>()...));

    } // namespace Traits

    /**
     * @brief Reusable query CPO base with a compile-time fallback.
     * @details The selected query tag is constructed as a temporary and passed to a const environment lvalue. When
     * that exact expression is unsupported, @p Default is returned by value.
     * @tparam Tag Default query tag.
     * @tparam Default Fallback value returned when the environment does not answer the query.
     * @par Example
     * @code{.cpp}
     * struct GetPriority {};
     * struct Environment { constexpr int Query(GetPriority) const noexcept { return 8; } };
     * struct EmptyEnvironment {};
     * inline constexpr Sora::Exec::QueryBase<GetPriority, 0> priority{};
     * static_assert(priority(Environment{}) == 8);      // supported input -> query output 8
     * static_assert(priority(EmptyEnvironment{}) == 0); // unsupported input -> fallback output 0
     * @endcode
     */
    template<Concept::QueryableTag Tag, auto Default = std::ignore>
    struct QueryBase {
        template<Concept::QueryableTag InnerTag = Tag, typename Env, typename... Args>
        requires Concept::QueryableFor<Env, InnerTag, Args...> || std::copy_constructible<decltype(Default)>
        [[nodiscard]] constexpr decltype(auto) operator()(const Env& env, Args&&... args) const
            noexcept(Concept::QueryableFor<Env, InnerTag, Args...>
                         ? Concept::NothrowQueryableFor<Env, InnerTag, Args...>
                         : std::is_nothrow_copy_constructible_v<decltype(Default)>) {
            if constexpr (Concept::QueryableFor<Env, InnerTag, Args...>) {
                return Detail::InvokeQuery<InnerTag, Env, Args...>(env, std::forward<Args>(args)...);
            } else {
                return Default;
            }
        }
    };

    /**
     * @brief Stateless base marking a query tag as forwardable.
     * @details Inheritance from this type is the sole forwarding opt-in mechanism; query-member customization is not
     * consulted.
     * @par Example
     * @code{.cpp}
     * struct GetScheduler : Sora::Exec::ForwardingQueryMarker {};
     * static_assert(Sora::Exec::ForwardingQuery(GetScheduler{})); // input marked tag -> output true
     * @endcode
     */
    struct ForwardingQueryMarker {};

    /**
     * @brief Detect whether a Sora query tag opts into forwarding.
     * @details The CPO returns the marker truth value itself, not merely whether a detection expression is valid.
     * @par Example
     * @code{.cpp}
     * struct GetScheduler : Sora::Exec::ForwardingQueryMarker {};
     * struct GetPriority {};
     * static_assert(Sora::Exec::ForwardingQuery(GetScheduler{})); // input marked tag -> output true
     * static_assert(!Sora::Exec::ForwardingQuery(GetPriority{})); // input ordinary tag -> output false
     * @endcode
     */
    struct ForwardingQueryTag {
        template<Concept::QueryableTag QueryTag>
        consteval bool operator()(QueryTag) const noexcept {
            return std::derived_from<std::remove_cvref_t<QueryTag>, ForwardingQueryMarker>;
        }
    };

    /** @brief Sora-private forwarding-marker query CPO. */
    inline constexpr ForwardingQueryTag ForwardingQuery{};

    namespace Concept {

        /**
         * @brief Whether @p T is a query tag positively marked for forwarding.
         * @tparam T Candidate query tag.
         * @par Example
         * @code{.cpp}
         * struct Forwarded : Sora::Exec::ForwardingQueryMarker {};
         * struct Local {};
         * static_assert(Sora::Exec::Concept::ForwardingQueryableTag<Forwarded>); // input Forwarded -> true
         * static_assert(!Sora::Exec::Concept::ForwardingQueryableTag<Local>);     // input Local -> false
         * @endcode
         */
        template<typename T>
        concept ForwardingQueryableTag = QueryableTag<T> && ForwardingQuery(std::remove_cvref_t<T>{});

    } // namespace Concept

    /**
     * @brief Invoke a callable query or return a supplied fallback value.
     * @details This Sora adapter invokes @p queryer directly. When invocation is unsupported, it constructs and
     * returns std::decay_t<Default> from @p defaultValue. A move-only fallback must therefore be supplied as an
     * rvalue; an lvalue fallback is accepted only when its decayed type is constructible from that lvalue.
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
        requires(!std::invocable<Queryer, Env, Args...>) &&
                std::constructible_from<std::decay_t<Default>, Default&&>
        [[nodiscard]] constexpr auto operator()(Queryer&&, Env&&, Default&& defaultValue, Args&&...) const
            noexcept(std::is_nothrow_constructible_v<std::decay_t<Default>, Default&&>) -> std::decay_t<Default> {
            return std::forward<Default>(defaultValue);
        }
    };

    /** @brief Sora-private callable-or-fallback adapter. */
    inline constexpr QueryOrTag QueryOr{};

    namespace Traits {

        /**
         * @brief Result type of the exact invocation selected by @ref QueryOr.
         * @tparam Queryer Callable candidate type.
         * @tparam Env Input object type.
         * @tparam Default Fallback expression type.
         * @tparam Args Additional argument types.
         * @par Example
         * @code{.cpp}
         * struct Unsupported {};
         * using Result = Sora::Exec::Traits::QueryOrResultType<Unsupported, int, const char (&)[5]>;
         * static_assert(std::same_as<Result, const char*>); // unsupported input -> decayed fallback type
         * @endcode
         */
        template<typename Queryer, typename Env, typename Default, typename... Args>
        requires requires {
            QueryOr(std::declval<Queryer>(), std::declval<Env>(), std::declval<Default>(), std::declval<Args>()...);
        }
        using QueryOrResultType = decltype(
            QueryOr(std::declval<Queryer>(), std::declval<Env>(), std::declval<Default>(), std::declval<Args>()...));

    } // namespace Traits

} // namespace Sora::Concurrency::Exec

namespace Sora {

    namespace Exec = Concurrency::Exec;

    namespace Concept {

        inline namespace Exec {

            /**
             * @brief Public facade for Sora execution-environment eligibility.
             * @tparam T Candidate environment type.
             * @par Example
             * @code{.cpp}
             * struct Environment {};
             * static_assert(Sora::Concept::Queryable<Environment>); // input Environment -> output true
             * @endcode
             */
            template<typename T>
            concept Queryable = Sora::Exec::Concept::Queryable<T>;

            /**
             * @brief Public facade for Sora query support.
             * @tparam Env Environment type.
             * @tparam QueriedTag Stateless query identity.
             * @tparam Args Additional query argument types.
             * @par Example
             * @code{.cpp}
             * struct GetValue {};
             * struct Environment { constexpr int Query(GetValue) const noexcept { return 2; } };
             * static_assert(Sora::Concept::QueryableFor<Environment, GetValue>); // input pair -> output true
             * @endcode
             */
            template<typename Env, typename QueriedTag, typename... Args>
            concept QueryableFor = Sora::Exec::Concept::QueryableFor<Env, QueriedTag, Args...>;

            /**
             * @brief Public facade for non-throwing Sora query support.
             * @tparam Env Environment type.
             * @tparam QueriedTag Stateless query identity.
             * @tparam Args Additional query argument types.
             * @par Example
             * @code{.cpp}
             * struct GetValue {};
             * struct Environment { constexpr int Query(GetValue) const noexcept { return 2; } };
             * static_assert(Sora::Concept::NothrowQueryableFor<Environment, GetValue>); // input pair -> output true
             * @endcode
             */
            template<typename Env, typename QueriedTag, typename... Args>
            concept NothrowQueryableFor = Sora::Exec::Concept::NothrowQueryableFor<Env, QueriedTag, Args...>;

        } // namespace Exec

        inline namespace Concurrency {

            namespace Exec = Sora::Concurrency::Exec::Concept;

        } // namespace Concurrency

    } // namespace Concept

} // namespace Sora
