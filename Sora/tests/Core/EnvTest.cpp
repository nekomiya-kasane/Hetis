#include <Sora/Core/Concurrency/Executation/Env.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace {

    struct GetValue {};
    struct Missing {};

    struct Environment {
        constexpr int Query(GetValue) const noexcept { return 11; }
    };

    struct Provider {
        constexpr Environment GetEnv() const noexcept { return {}; }
    };

    struct ThrowingProvider {
        Environment GetEnv() const { return {}; }
    };

    struct MutableProvider {
        Environment GetEnv() noexcept { return {}; }
    };

    struct RvalueQuery {};

    struct RvalueEnvironment {
        constexpr int Query(RvalueQuery&&) const noexcept { return 13; }
    };

    struct ThrowingQuery {};

    struct ThrowingEnvironment {
        int Query(ThrowingQuery) const { return 19; }
    };

    struct ArgumentQuery {};

    struct ArgumentEnvironment {
        int Query(ArgumentQuery, std::unique_ptr<int>&& value) const noexcept { return *value; }
    };

    struct ReferenceQuery {};

    struct ReferenceEnvironment {
        int value = 31;

        constexpr const int& Query(ReferenceQuery) const noexcept { return value; }
    };

    struct NonDefaultQuery {
        NonDefaultQuery() = delete;
    };

    struct NonConstexprQuery {
        NonConstexprQuery() {}
    };

    struct StatefulQuery {
        int channel;
    };

    template<typename Queryer, typename Env, typename Default, typename... Args>
    concept CanQueryOr = requires(Queryer&& queryer, Env&& env, Default&& defaultValue, Args&&... args) {
        Sora::Exec::QueryOr(std::forward<Queryer>(queryer), std::forward<Env>(env),
                            std::forward<Default>(defaultValue), std::forward<Args>(args)...);
    };

    static_assert(Sora::Exec::Concept::Queryable<Environment>);
    static_assert(Sora::Exec::Concept::QueryableFor<Environment, GetValue>);
    static_assert(Sora::Exec::Concept::NothrowQueryableFor<Environment, GetValue>);
    static_assert(Sora::Exec::Concept::EnvProvider<Provider>);
    static_assert(!Sora::Exec::Concept::EnvProvider<ThrowingProvider>);
    static_assert(!Sora::Exec::Concept::EnvProvider<MutableProvider>);
    static_assert(Sora::Exec::GetEnv(Provider{}).Query(GetValue{}) == 11);
    static_assert(Sora::Exec::Concept::QueryableFor<RvalueEnvironment, RvalueQuery>);
    static_assert(Sora::Exec::QueryBase<RvalueQuery, 0>{}(RvalueEnvironment{}) == 13);
    static_assert(Sora::Exec::Concept::QueryableFor<ThrowingEnvironment, ThrowingQuery>);
    static_assert(!Sora::Exec::Concept::NothrowQueryableFor<ThrowingEnvironment, ThrowingQuery>);
    static_assert(Sora::Exec::Concept::QueryableFor<ArgumentEnvironment, ArgumentQuery, std::unique_ptr<int>>);
    static_assert(!Sora::Exec::Concept::QueryableFor<ArgumentEnvironment, ArgumentQuery, std::unique_ptr<int>&>);
    static_assert(std::same_as<Sora::Exec::Traits::QueryableResultType<ReferenceEnvironment, ReferenceQuery>,
                               const int&>);
    static_assert(std::same_as<
                  decltype(Sora::Exec::QueryBase<ReferenceQuery>{}(std::declval<const ReferenceEnvironment&>())),
                  const int&>);
    static_assert(!Sora::Exec::Concept::QueryableTag<NonDefaultQuery>);
    static_assert(!Sora::Exec::Concept::QueryableTag<NonConstexprQuery>);
    static_assert(!Sora::Exec::Concept::QueryableTag<StatefulQuery>);
    static_assert(std::same_as<Sora::Exec::Traits::QueryOrResultType<Missing, Environment, const char (&)[8]>,
                               const char*>);
    static_assert(CanQueryOr<Missing, Environment, std::unique_ptr<int>>);
    static_assert(!CanQueryOr<Missing, Environment, std::unique_ptr<int>&>);

    struct Priority : Sora::Exec::ForwardingQueryMarker {};

    static_assert(Sora::Exec::Concept::ForwardingQueryableTag<Priority>);
    static_assert(!Sora::Exec::Concept::ForwardingQueryableTag<Missing>);
    static_assert(Sora::Exec::ForwardingQuery(Priority{}));
    static_assert(!Sora::Exec::ForwardingQuery(Missing{}));

} // namespace

TEST_CASE("execution environment queries") {
    constexpr Sora::Exec::QueryBase<GetValue, 0> getValue;
    constexpr Sora::Exec::QueryBase<Missing, 17> missing;

    REQUIRE(getValue(Environment{}) == 11);
    REQUIRE(missing(Environment{}) == 17);
    REQUIRE(Sora::Exec::QueryBase<ArgumentQuery>{}(ArgumentEnvironment{}, std::make_unique<int>(37)) == 37);
    REQUIRE(Sora::Exec::QueryOr([](int value) { return value * 2; }, 6, 0) == 12);
    REQUIRE(Sora::Exec::QueryOr([](const Environment&) -> int { return 1; }, Environment{}, 0) == 1);
    REQUIRE(Sora::Exec::QueryOr([](const Missing&) -> int { return 1; }, Environment{}, 23) == 23);
    auto fallback = Sora::Exec::QueryOr(Missing{}, Environment{}, std::make_unique<int>(29));
    REQUIRE(*fallback == 29);
    REQUIRE(Sora::Exec::ForwardingQuery(Priority{}));
}
