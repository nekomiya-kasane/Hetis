/**
 * @file PlatformBackendTest.cpp
 * @brief Tests for PlatformBackend callback-view contracts.
 */
#include <Mashiro/Platform/PlatformBackend.h>

#include <Support/Meta.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <variant>

using namespace Mashiro;

namespace {

    struct NoThrowConsumer {
        void operator()(SystemEvent&&) noexcept {}
    };

    struct ThrowingConsumer {
        void operator()(SystemEvent&&) {}
    };

    struct WrongSignature {
        void operator()(int) noexcept {}
    };

} // namespace

TEST_CASE("SystemEventConsumer accepts only noexcept SystemEvent consumers", AUTO_TAG) {
    STATIC_REQUIRE(Platform::SystemEventConsumer<NoThrowConsumer>);
    STATIC_REQUIRE_FALSE(Platform::SystemEventConsumer<ThrowingConsumer>);
    STATIC_REQUIRE_FALSE(Platform::SystemEventConsumer<WrongSignature>);
}

TEST_CASE("SystemEventConsumerRef forwards system events without owning the callable", AUTO_TAG) {
    bool received = false;
    auto consumer = [&received](SystemEvent&& event) noexcept {
        received = std::holds_alternative<Event::WindowCloseEvent>(event);
    };

    Platform::SystemEventConsumerRef ref{consumer};
    ref(SystemEvent{Event::WindowCloseEvent{}});

    REQUIRE(received);
}