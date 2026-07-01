/**
 * @file EventChannelTest.cpp
 * @brief Tests for Platform/EventChannel.h batch-drain semantics.
 */
#include <Mashiro/Platform/EventChannel.h>

#include <Support/Meta.h>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <tuple>
#include <thread>
#include <variant>

using namespace Mashiro;

namespace {

    enum class Completion {
        None,
        Value,
        Stopped,
        Error,
    };

    struct StopAwareReceiver {
        std::atomic<Completion>* completion{};
        stdexec::inplace_stop_token token{};

        using receiver_concept = stdexec::receiver_t;

        [[nodiscard]] friend auto get_env(const StopAwareReceiver& self) noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, self.token}};
        }

        friend void set_value(StopAwareReceiver&& self, Platform::EventBatch) noexcept {
            self.completion->store(Completion::Value, std::memory_order_release);
        }

        friend void set_stopped(StopAwareReceiver&& self) noexcept {
            self.completion->store(Completion::Stopped, std::memory_order_release);
        }

        friend void set_error(StopAwareReceiver&& self, std::exception_ptr) noexcept {
            self.completion->store(Completion::Error, std::memory_order_release);
        }
    };

} // namespace

TEST_CASE("EventChannel NextBatch returns one atomic drain batch", AUTO_TAG) {
    Platform::EventChannel channel;

    REQUIRE(channel.TryPush(SystemEvent{Event::WindowCloseEvent{}}));
    REQUIRE(channel.TryPush(SystemEvent{Event::PowerResumeEvent{}}));

    auto result = stdexec::sync_wait(channel.NextBatch());

    REQUIRE(result.has_value());
    const auto& batch = std::get<0>(*result);
    REQUIRE(batch.size() == 2);
    REQUIRE(std::holds_alternative<Event::WindowCloseEvent>(batch[0]));
    REQUIRE(std::holds_alternative<Event::PowerResumeEvent>(batch[1]));
    REQUIRE(channel.Empty());
}

TEST_CASE("EventChannel NextBatch completes stopped when stop is requested while parked", AUTO_TAG) {
    using namespace std::chrono_literals;

    Platform::EventChannel channel;
    stdexec::inplace_stop_source stopSource;
    std::atomic<Completion> completion{Completion::None};

    std::jthread waiter{[&] {
        auto operation = stdexec::connect(channel.NextBatch(), StopAwareReceiver{
            .completion = &completion,
            .token = stopSource.get_token(),
        });
        stdexec::start(operation);
    }};

    std::this_thread::sleep_for(50ms);
    stopSource.request_stop();

    for (int i = 0; i != 20 && completion.load(std::memory_order_acquire) == Completion::None; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    if (completion.load(std::memory_order_acquire) == Completion::None) {
        REQUIRE(channel.TryPush(SystemEvent{Event::PowerResumeEvent{}}));
    }

    waiter.join();
    REQUIRE(completion.load(std::memory_order_acquire) == Completion::Stopped);
}
