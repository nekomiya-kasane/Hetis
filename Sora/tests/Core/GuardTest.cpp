#include <Sora/Core/Guard.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

    struct EmptyAction {
        constexpr void operator()() const noexcept {}
    };

    struct CopyPreferredAction {
        int* calls;
        int* copies;
        int* moves;

        CopyPreferredAction(int& callCount, int& copyCount, int& moveCount) noexcept
            : calls{&callCount}, copies{&copyCount}, moves{&moveCount} {}

        CopyPreferredAction(const CopyPreferredAction& other) noexcept
            : calls{other.calls}, copies{other.copies}, moves{other.moves} {
            ++*copies;
        }

        CopyPreferredAction(CopyPreferredAction&& other) noexcept(false)
            : calls{other.calls}, copies{other.copies}, moves{other.moves} {
            ++*moves;
            throw std::runtime_error{"move must not be selected"};
        }

        void operator()() const noexcept { ++*calls; }
    };

    struct ThrowingCopyAction {
        int* calls;

        explicit ThrowingCopyAction(int& callCount) noexcept : calls{&callCount} {}
        ThrowingCopyAction(const ThrowingCopyAction&) { throw std::runtime_error{"copy failed"}; }
        ThrowingCopyAction(ThrowingCopyAction&&) noexcept(false) { throw std::runtime_error{"move failed"}; }

        void operator()() const noexcept { ++*calls; }
    };

    struct ThrowingSuccessAction {
        void operator()() const { throw std::runtime_error{"success callback failed"}; }
    };

} // namespace

static_assert(!std::copy_constructible<Sora::ScopeExit<EmptyAction>>);
static_assert(std::move_constructible<Sora::ScopeExit<EmptyAction>>);
static_assert(std::is_nothrow_destructible_v<Sora::ScopeExit<EmptyAction>>);
static_assert(!std::is_nothrow_destructible_v<Sora::ScopeSuccess<ThrowingSuccessAction>>);
static_assert(sizeof(Sora::ScopeExit<EmptyAction>) <= 2);

TEST_CASE("ScopeExit invokes once and can be released", "[Sora][Core][Guard]") {
    int calls = 0;
    {
        Sora::ScopeExit guard{[&] noexcept { ++calls; }};
    }
    REQUIRE(calls == 1);

    {
        Sora::ScopeExit guard{[&] noexcept { ++calls; }};
        guard.Release();
    }
    REQUIRE(calls == 1);
}

TEST_CASE("Scope guards copy instead of using a potentially throwing move", "[Sora][Core][Guard]") {
    int calls = 0;
    int copies = 0;
    int moves = 0;
    CopyPreferredAction action{calls, copies, moves};

    {
        Sora::ScopeExit<CopyPreferredAction> first{action};
        Sora::ScopeExit<CopyPreferredAction> second{std::move(first)};
    }

    REQUIRE(calls == 1);
    REQUIRE(copies == 2);
    REQUIRE(moves == 0);
}

TEST_CASE("ScopeExit invokes the source callback when callback storage construction fails", "[Sora][Core][Guard]") {
    int calls = 0;
    ThrowingCopyAction action{calls};

    REQUIRE_THROWS_AS(Sora::ScopeExit<ThrowingCopyAction>{action}, std::runtime_error);
    REQUIRE(calls == 1);
}

TEST_CASE("ScopeFail distinguishes new exception unwinding", "[Sora][Core][Guard]") {
    int calls = 0;
    {
        Sora::ScopeFail guard{[&] noexcept { ++calls; }};
    }
    REQUIRE(calls == 0);

    REQUIRE_THROWS_AS(
        [&] {
            Sora::ScopeFail guard{[&] noexcept { ++calls; }};
            throw std::runtime_error{"failure"};
        }(),
        std::runtime_error);
    REQUIRE(calls == 1);

    try {
        throw std::runtime_error{"outer"};
    } catch (...) {
        Sora::ScopeFail guard{[&] noexcept { ++calls; }};
    }
    REQUIRE(calls == 1);

    {
        Sora::ScopeFail guard{[&] noexcept { ++calls; }};
        guard.Release();
    }
    REQUIRE(calls == 1);
}

TEST_CASE("ScopeSuccess runs only after successful completion", "[Sora][Core][Guard]") {
    int calls = 0;
    {
        Sora::ScopeSuccess guard{[&] noexcept { ++calls; }};
    }
    REQUIRE(calls == 1);

    REQUIRE_THROWS_AS(
        [&] {
            Sora::ScopeSuccess guard{[&] noexcept { ++calls; }};
            throw std::runtime_error{"failure"};
        }(),
        std::runtime_error);
    REQUIRE(calls == 1);

    REQUIRE_THROWS_AS([] { Sora::ScopeSuccess guard{ThrowingSuccessAction{}}; }(), std::runtime_error);
}

TEST_CASE("ScopeSuccess does not invoke the source callback when storage construction fails", "[Sora][Core][Guard]") {
    int calls = 0;
    ThrowingCopyAction action{calls};

    REQUIRE_THROWS_AS(Sora::ScopeSuccess<ThrowingCopyAction>{action}, std::runtime_error);
    REQUIRE(calls == 0);
}
