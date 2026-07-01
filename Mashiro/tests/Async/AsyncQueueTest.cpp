/**
 * @file AsyncQueueTest.cpp
 * @brief Tests for the generic sender facade over Pop/PopBatch/Push/PushBatch queue contracts.
 */
#include <Mashiro/Concurrency/AsyncQueue.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>

using namespace Mashiro;
using namespace std::chrono_literals;

namespace {

    struct TestBatch {
        static constexpr std::size_t kCapacity = 8;

        std::array<int, kCapacity> values{};
        std::size_t count{0};

        TestBatch() noexcept = default;

        TestBatch(std::initializer_list<int> init) noexcept {
            for (int value : init) {
                Push(value);
            }
        }

        [[nodiscard]] bool empty() const noexcept { return count == 0; }
        [[nodiscard]] std::size_t size() const noexcept { return count; }

        void Push(int value) noexcept {
            values[count] = value;
            ++count;
        }

        void clear() noexcept { count = 0; }

        [[nodiscard]] friend bool operator==(const TestBatch& lhs, const TestBatch& rhs) noexcept {
            if (lhs.count != rhs.count) {
                return false;
            }
            for (std::size_t i = 0; i != lhs.count; ++i) {
                if (lhs.values[i] != rhs.values[i]) {
                    return false;
                }
            }
            return true;
        }
    };

    class TestQueue {
    public:
        using value_type = int;
        using batch_type = TestBatch;

        explicit TestQueue(std::size_t capacity) noexcept : capacity_(capacity) {}

        [[nodiscard]] bool Push(value_type& value) noexcept {
            std::scoped_lock lock{mutex_};
            if (size_ == capacity_) {
                return false;
            }
            slots_[Tail()] = value;
            ++size_;
            return true;
        }

        [[nodiscard]] bool PushBatch(batch_type& batch) noexcept {
            std::scoped_lock lock{mutex_};
            if (batch.empty()) {
                return true;
            }
            if (capacity_ - size_ < batch.size()) {
                return false;
            }
            for (std::size_t i = 0; i != batch.size(); ++i) {
                slots_[Tail()] = batch.values[i];
                ++size_;
            }
            batch.clear();
            return true;
        }

        [[nodiscard]] std::optional<value_type> Pop() noexcept {
            std::scoped_lock lock{mutex_};
            if (size_ == 0) {
                return std::nullopt;
            }
            int value = slots_[head_];
            head_ = (head_ + 1) % capacity_;
            --size_;
            return value;
        }

        [[nodiscard]] batch_type PopBatch() noexcept {
            std::scoped_lock lock{mutex_};
            batch_type batch;
            while (size_ != 0) {
                batch.Push(slots_[head_]);
                head_ = (head_ + 1) % capacity_;
                --size_;
            }
            return batch;
        }

        [[nodiscard]] bool IsEmpty() const noexcept {
            std::scoped_lock lock{mutex_};
            return size_ == 0;
        }

    private:
        [[nodiscard]] std::size_t Tail() const noexcept { return (head_ + size_) % capacity_; }

        std::size_t capacity_{0};
        mutable std::mutex mutex_;
        std::array<int, TestBatch::kCapacity> slots_{};
        std::size_t head_{0};
        std::size_t size_{0};
    };

} // namespace

TEST_CASE("AsyncQueue Pop completes after a later Push", AUTO_TAG) {
    TestQueue storage{4};
    AsyncQueue queue{storage};
    std::atomic<bool> consumerStarted{false};
    std::optional<int> received;

    std::thread consumer{[&] {
        consumerStarted.store(true, std::memory_order_release);
        auto result = stdexec::sync_wait(queue.Pop());
        if (result.has_value()) {
            received = std::get<0>(*result);
        }
    }};

    while (!consumerStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(10ms);

    REQUIRE(stdexec::sync_wait(queue.Push(42)).has_value());

    consumer.join();
    REQUIRE(received.has_value());
    REQUIRE(*received == 42);
    REQUIRE(queue.IsEmpty());
}

TEST_CASE("AsyncQueue PopBatch completes with the current batch", AUTO_TAG) {
    TestQueue storage{8};
    AsyncQueue queue{storage};

    REQUIRE(stdexec::sync_wait(queue.Push(1)).has_value());
    REQUIRE(stdexec::sync_wait(queue.Push(2)).has_value());

    auto result = stdexec::sync_wait(queue.PopBatch());
    REQUIRE(result.has_value());
    const auto& batch = std::get<0>(*result);
    REQUIRE(batch == TestBatch{1, 2});
    REQUIRE(queue.IsEmpty());
}

TEST_CASE("AsyncQueue Push waits until a Pop opens space", AUTO_TAG) {
    TestQueue storage{1};
    AsyncQueue queue{storage};

    REQUIRE(stdexec::sync_wait(queue.Push(1)).has_value());

    std::atomic<bool> producerStarted{false};
    std::atomic<bool> producerFinished{false};
    std::thread producer{[&] {
        producerStarted.store(true, std::memory_order_release);
        auto result = stdexec::sync_wait(queue.Push(2));
        producerFinished.store(result.has_value(), std::memory_order_release);
    }};

    while (!producerStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(10ms);
    REQUIRE_FALSE(producerFinished.load(std::memory_order_acquire));

    auto first = stdexec::sync_wait(queue.Pop());
    REQUIRE(first.has_value());
    REQUIRE(std::get<0>(*first) == 1);

    producer.join();
    REQUIRE(producerFinished.load(std::memory_order_acquire));

    auto second = stdexec::sync_wait(queue.Pop());
    REQUIRE(second.has_value());
    REQUIRE(std::get<0>(*second) == 2);
}

TEST_CASE("AsyncQueue PushBatch waits for enough capacity before consuming the batch", AUTO_TAG) {
    TestQueue storage{4};
    AsyncQueue queue{storage};

    REQUIRE(stdexec::sync_wait(queue.Push(1)).has_value());
    REQUIRE(stdexec::sync_wait(queue.Push(2)).has_value());
    REQUIRE(stdexec::sync_wait(queue.Push(3)).has_value());

    TestBatch pending{10, 20};
    std::atomic<bool> batchFinished{false};
    std::thread producer{[&] {
        auto result = stdexec::sync_wait(queue.PushBatch(std::move(pending)));
        batchFinished.store(result.has_value(), std::memory_order_release);
    }};

    std::this_thread::sleep_for(10ms);
    REQUIRE_FALSE(batchFinished.load(std::memory_order_acquire));

    auto batchBeforeSpace = stdexec::sync_wait(queue.PopBatch());
    REQUIRE(batchBeforeSpace.has_value());
    REQUIRE(std::get<0>(*batchBeforeSpace) == TestBatch{1, 2, 3});

    producer.join();
    REQUIRE(batchFinished.load(std::memory_order_acquire));

    auto batchAfterSpace = stdexec::sync_wait(queue.PopBatch());
    REQUIRE(batchAfterSpace.has_value());
    REQUIRE(std::get<0>(*batchAfterSpace) == TestBatch{10, 20});
}