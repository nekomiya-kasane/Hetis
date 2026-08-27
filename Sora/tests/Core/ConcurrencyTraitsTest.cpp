/**
 * @file ConcurrencyTraitsTest.cpp
 * @brief Compile-time validation for coroutine protocol traits.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/Traits.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

namespace {

    struct Promise;

    struct ExplicitBoolean {
        explicit operator bool() const noexcept { return true; }
    };

    struct VoidAwaiter {
        ExplicitBoolean await_ready() const noexcept { return {}; }
        void await_suspend(std::coroutine_handle<>) noexcept {}
        int& await_resume() noexcept;
    };

    struct BoolAwaiter {
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<>) noexcept { return false; }
        void await_resume() noexcept {}
    };

    struct TransferAwaiter {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept { return std::noop_coroutine(); }
        void await_resume() noexcept {}
    };

    struct TypedAwaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<Promise>) noexcept {}
        void await_resume() noexcept {}
    };

    struct GenericAwaiter {
        bool await_ready() const noexcept { return false; }

        template<typename AwaitingPromise>
        void await_suspend(std::coroutine_handle<AwaitingPromise>) noexcept {}

        void await_resume() noexcept {}
    };

    struct InvalidSuspendResultAwaiter {
        bool await_ready() const noexcept { return false; }
        int await_suspend(std::coroutine_handle<>) noexcept { return 0; }
        void await_resume() noexcept {}
    };

    struct ReferenceSuspendResultAwaiter {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<>& await_suspend(std::coroutine_handle<>) noexcept;
        void await_resume() noexcept {}
    };

    struct ThrowingAwaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) {}
        void await_resume() noexcept {}
    };

    struct MixedMembersAwaiter {
        int unrelated = 0;

        bool await_ready() const noexcept { return false; }
        int await_suspend(int) noexcept { return 0; }
        void await_suspend(std::coroutine_handle<>) noexcept {}
        void await_resume() noexcept {}
    };

    struct MemberAwaitable {
        VoidAwaiter operator co_await() const noexcept { return {}; }
    };

    struct AwaiterWithInvalidOperator : VoidAwaiter {
        int operator co_await() const noexcept { return 0; }
    };

    namespace Adl {

        struct Awaitable {};

        [[maybe_unused]] inline BoolAwaiter operator co_await(Awaitable) noexcept { return {}; }

        struct DualAwaitable {
            VoidAwaiter operator co_await() const noexcept { return {}; }
        };

        [[maybe_unused]] inline BoolAwaiter operator co_await(DualAwaitable) noexcept { return {}; }

    } // namespace Adl

    struct Promise {
        struct ReturnObject;

        ReturnObject get_return_object() noexcept;
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept {}
        void return_void() noexcept {}

        template<typename T>
        std::suspend_always yield_value(T&&) noexcept {
            return {};
        }

        template<typename T>
        T&& await_transform(T&& value) noexcept {
            return std::forward<T>(value);
        }
    };

    struct Promise::ReturnObject {
        using promise_type = Promise;
    };

    Promise::ReturnObject Promise::get_return_object() noexcept { return {}; }

    [[maybe_unused]] Promise::ReturnObject CompileGeneratorProtocol() {
        co_yield 42;
    }

    struct ValuePromise {
        struct ReturnObject;

        ReturnObject get_return_object() noexcept;
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept {}

        template<typename T>
        void return_value(T&&) noexcept {}
    };

    struct ValuePromise::ReturnObject {
        using promise_type = ValuePromise;
    };

    ValuePromise::ReturnObject ValuePromise::get_return_object() noexcept { return {}; }

    [[maybe_unused]] ValuePromise::ReturnObject CompileValueReturnProtocol() {
        co_return 42;
    }

    struct ThrowingFinalPromise {
        void get_return_object() noexcept {}
        std::suspend_never initial_suspend() noexcept { return {}; }
        ThrowingAwaiter final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept {}
        void return_void() noexcept {}
    };

    struct ConflictingReturnPromise {
        void get_return_object() noexcept {}
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept {}
        void return_void() noexcept {}
        void return_value(int) noexcept {}
    };

} // namespace

TEST_CASE("Coroutine handle concepts preserve erased and typed identity", "[Sora][Core][CoroutineTraits]") {
    STATIC_REQUIRE(Sora::Concept::CoroutineHandle<std::coroutine_handle<>>);
    STATIC_REQUIRE(Sora::Concept::CoroutineHandle<const std::coroutine_handle<Promise>&>);
    STATIC_REQUIRE(Sora::Concept::TypeErasedCoroutineHandle<std::coroutine_handle<>>);
    STATIC_REQUIRE_FALSE(Sora::Concept::TypeErasedCoroutineHandle<std::coroutine_handle<Promise>>);
    STATIC_REQUIRE(Sora::Concept::TypedCoroutineHandle<std::coroutine_handle<Promise>>);
    STATIC_REQUIRE_FALSE(Sora::Concept::CoroutineHandle<int>);
}

TEST_CASE("Awaiter concepts enforce the standard suspend result categories", "[Sora][Core][CoroutineTraits]") {
    STATIC_REQUIRE(Sora::Concept::Awaiter<VoidAwaiter>);
    STATIC_REQUIRE(Sora::Concept::Awaiter<BoolAwaiter>);
    STATIC_REQUIRE(Sora::Concept::Awaiter<TransferAwaiter>);
    STATIC_REQUIRE(Sora::Concept::Awaiter<GenericAwaiter>);
    STATIC_REQUIRE(Sora::Concept::Awaiter<ThrowingAwaiter>);
    STATIC_REQUIRE_FALSE(Sora::Concept::NothrowAwaiter<ThrowingAwaiter>);
    STATIC_REQUIRE(Sora::Concept::AwaiterFor<TypedAwaiter, Promise>);
    STATIC_REQUIRE_FALSE(Sora::Concept::Awaiter<TypedAwaiter>);
    STATIC_REQUIRE_FALSE(Sora::Concept::Awaiter<InvalidSuspendResultAwaiter>);
    STATIC_REQUIRE_FALSE(Sora::Concept::Awaiter<ReferenceSuspendResultAwaiter>);

    STATIC_REQUIRE(std::same_as<Sora::Traits::AwaitResultType<VoidAwaiter>, int&>);
    STATIC_REQUIRE(Sora::Traits::AwaitSuspendSignatureOf<VoidAwaiter> ==
                   Sora::Traits::AwaitSuspendSignature::VoidHandle);
    STATIC_REQUIRE(Sora::Traits::AwaitSuspendSignatureOf<BoolAwaiter> ==
                   Sora::Traits::AwaitSuspendSignature::BoolHandle);
    STATIC_REQUIRE(Sora::Traits::AwaitSuspendSignatureOf<TransferAwaiter> ==
                   Sora::Traits::AwaitSuspendSignature::HandleHandle);
}

TEST_CASE("Awaitable concepts recognize direct member and ADL protocols", "[Sora][Core][CoroutineTraits]") {
    STATIC_REQUIRE(Sora::Concept::AwaitableType<VoidAwaiter>);
    STATIC_REQUIRE(Sora::Concept::AwaitableType<MemberAwaitable>);
    STATIC_REQUIRE(Sora::Concept::AwaitableType<Adl::Awaitable>);
    STATIC_REQUIRE_FALSE(Sora::Concept::AwaitableType<AwaiterWithInvalidOperator>);
    STATIC_REQUIRE_FALSE(Sora::Concept::AwaitableType<Adl::DualAwaitable>);
    STATIC_REQUIRE_FALSE(Sora::Concept::AwaitableType<int>);
}

TEST_CASE("Reflection queries preserve traversal and reject invalid suspend overloads",
          "[Sora][Core][CoroutineTraits]") {
    constexpr auto suspenders = Sora::Meta::AwaitSuspendFunctionsOf(^^MixedMembersAwaiter);
    STATIC_REQUIRE(suspenders.size() == 1);
    STATIC_REQUIRE(std::meta::identifier_of(suspenders.front()) == "await_suspend");
    STATIC_REQUIRE(std::meta::return_type_of(suspenders.front()) == ^^void);
    STATIC_REQUIRE(Sora::Meta::AwaitSuspendFunctionsOf(^^int).empty());
    STATIC_REQUIRE(Sora::Meta::CoroutineReturnFunctionsOf(^^int).empty());
}

TEST_CASE("Promise capabilities keep yield transform and completion orthogonal", "[Sora][Core][CoroutineTraits]") {
    STATIC_REQUIRE(Sora::Concept::PromiseType<Promise>);
    STATIC_REQUIRE(Sora::Concept::PromiseTypeWithVoidReturn<Promise>);
    STATIC_REQUIRE(Sora::Concept::PromiseTypeWithYieldReturn<Promise>);
    STATIC_REQUIRE(Sora::Concept::PromiseTypeWithTransformer<Promise>);
    STATIC_REQUIRE(Sora::Concept::YieldingPromiseFor<Promise, int>);
    STATIC_REQUIRE(Sora::Concept::TransformingPromiseFor<Promise, std::suspend_always>);
    STATIC_REQUIRE(Sora::Concept::GeneratorType<Promise::ReturnObject>);
    STATIC_REQUIRE(Sora::Concept::CoroutineTypeWithVoidReturn<Promise::ReturnObject>);

    constexpr auto functions = Sora::Meta::CoroutineReturnFunctionsOf(^^Promise);
    STATIC_REQUIRE(functions.size() == 2);
    STATIC_REQUIRE(Sora::Traits::CoroutineReturnFunctionOf<Promise> ==
                   Sora::Traits::CoroutineReturnFunction::ReturnVoid);
}

TEST_CASE("Promise value-return capability accepts function templates", "[Sora][Core][CoroutineTraits]") {
    STATIC_REQUIRE(Sora::Concept::PromiseType<ValuePromise>);
    STATIC_REQUIRE(Sora::Concept::PromiseTypeWithValueReturn<ValuePromise>);
    STATIC_REQUIRE(Sora::Concept::ValueReturningPromiseFor<ValuePromise, int>);
    STATIC_REQUIRE(Sora::Concept::CoroutineTypeWithValueReturn<ValuePromise::ReturnObject>);
    STATIC_REQUIRE(Sora::Traits::CoroutineReturnFunctionOf<ValuePromise> ==
                   Sora::Traits::CoroutineReturnFunction::ReturnValue);
    STATIC_REQUIRE_FALSE(Sora::Concept::PromiseType<ThrowingFinalPromise>);
    STATIC_REQUIRE_FALSE(Sora::Concept::PromiseType<ConflictingReturnPromise>);
}
