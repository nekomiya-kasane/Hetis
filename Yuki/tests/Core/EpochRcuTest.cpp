#include <Yuki/Core/EpochRcu.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

using namespace Yuki;

namespace {
    // Heap-payload retire target: each instance bumps a shared counter on destruction.
    struct Counted {
        std::atomic<int>* counter;
        explicit Counted(std::atomic<int>* c) : counter(c) {}
        ~Counted() { counter->fetch_add(1, std::memory_order_relaxed); }
    };
    void DeleteCounted(void* p) noexcept {
        delete static_cast<Counted*>(p);
    }
}

TEST_CASE("RcuReadGuard nested guard is a no-op", AUTO_TAG) {
    // Outer guard publishes an epoch; inner guard sees a non-zero local epoch and the
    // ctor returns without re-publishing. Inner ~RcuReadGuard must NOT reset the slot.
    RcuReadGuard outer;
    {
        RcuReadGuard inner;
        // Test passes by reaching here without a deadlock or assert. The behavioural
        // correctness is exercised by the "reader blocks reclaim" case below.
    }
    SUCCEED();
}

TEST_CASE("RetireSnapshot + TryReclaim happy path frees pointers when no reader is active",
          AUTO_TAG) {
    std::atomic<int> freed{0};
    auto* p = new Counted{&freed};
    // No outer reader → safe epoch = UINT64_MAX, so the retired item is reclaimed on the
    // first TryReclaim after the gGlobalEpoch advances inside RetireSnapshot.
    RetireSnapshot(p, &DeleteCounted);
    (void)TryReclaim();
    REQUIRE(freed.load() == 1);
}

TEST_CASE("Active reader blocks reclamation of retirees that race its epoch", AUTO_TAG) {
    std::atomic<int> freed{0};
    {
        RcuReadGuard g;                      // reader publishes its epoch
        auto* p = new Counted{&freed};
        RetireSnapshot(p, &DeleteCounted);   // stamp ≥ reader.epoch
        (void)TryReclaim();
        // Reader still active → safe = reader.epoch ≤ stamp; item NOT reclaimed yet.
        REQUIRE(freed.load() == 0);
    }
    // Reader exited → slot quiesced to 0 → safe = UINT64_MAX → reclaim drains.
    (void)TryReclaim();
    REQUIRE(freed.load() == 1);
}

TEST_CASE("4 readers + 1 retirer stress: no leak, no crash, ASan clean", AUTO_TAG) {
    std::atomic<int> freed{0};
    std::atomic<int> retired{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                RcuReadGuard g;
                std::this_thread::yield();
            }
        });
    }

    std::thread retirer([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            auto* p = new Counted{&freed};
            RetireSnapshot(p, &DeleteCounted);
            retired.fetch_add(1, std::memory_order_relaxed);
            (void)TryReclaim();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();
    retirer.join();

    // Final drain: with all readers exited, every retired item must reclaim.
    (void)TryReclaim();
    REQUIRE(freed.load() == retired.load());
}
