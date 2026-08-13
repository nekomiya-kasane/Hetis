/**
 * @file ReclamationTest.cpp
 * @brief Verify stamped publication and safe-reclamation lifetime protocols.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/StampedValue.h>
#include <Sora/Core/Concurrency/Reclamation.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>

namespace {

    struct alignas(16) Node {
        int value = 0;
        inline static std::atomic<int> destroyed = 0;
        ~Node() { destroyed.fetch_add(1, std::memory_order_relaxed); }
    };

    enum class SlotState : std::uint8_t { Empty, Ready, Consumed };

    struct SlotStateCodec {
        using Value = SlotState;
        using Word = std::uint8_t;

        static constexpr Word PayloadMask = 0x03;
        static constexpr Word StampMask = 0xfc;
        static constexpr std::size_t StampShift = 2;

        [[nodiscard]] static constexpr Word Encode(Value value) noexcept { return static_cast<Word>(value); }
        [[nodiscard]] static constexpr Value Decode(Word word) noexcept {
            return static_cast<Value>(word & PayloadMask);
        }
    };

    static_assert(Sora::Concurrency::Concept::StampedWordCodec<SlotStateCodec>);

    template<typename Domain>
    void CheckDomainDrainsRetiredObjects() {
        Node::destroyed.store(0, std::memory_order_relaxed);
        {
            Domain domain;
            domain.Retire(new Node{1});
        }
        REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == 1);
    }

    template<typename Domain>
    void CheckBatchRetirementConsumesEachNonNullPointerOnce() {
        std::atomic<std::size_t> reclaimed = 0;
        {
            Domain domain;
            std::array pointers{new int{1}, static_cast<int*>(nullptr), new int{2}, new int{3}};
            domain.Retire(std::span{pointers}, [&](int* pointer) noexcept {
                delete pointer;
                reclaimed.fetch_add(1, std::memory_order_relaxed);
            });
            REQUIRE(domain.RetiredCount() == 3);
        }
        REQUIRE(reclaimed.load(std::memory_order_relaxed) == 3);
    }

} // namespace

TEST_CASE("Stamped values preserve pointer and bounded unsigned semantics",
          "[Sora.Core.Concurrency.Reclamation][Stamp]") {
    Node node{42};
    Sora::Concurrency::TaggedPointer<Node, 4> tagged{&node, 3};
    REQUIRE(tagged.Value() == &node);
    REQUIRE(tagged.Tag() == 3);
    REQUIRE(tagged.MaxTag() == 15);
    REQUIRE(tagged.WithTag(4).Tag() == 4);

    Sora::Concurrency::VersionedPointer<Node, 4> versioned{&node, 15};
    REQUIRE(versioned.Value() == &node);
    REQUIRE(versioned.MaxVersion() == 15);
    REQUIRE(versioned.NextVersion().Version() == 0);

    using VersionedIndex = Sora::Concurrency::VersionedUnsigned<std::uint32_t, 24, 8>;
    constexpr VersionedIndex compileTimeIndex{19, 7};
    STATIC_REQUIRE(compileTimeIndex.Value() == 19);
    STATIC_REQUIRE(compileTimeIndex.NextVersion().Version() == 8);
    VersionedIndex index{0x00ab'cdef, 7};
    REQUIRE(index.Value() == 0x00ab'cdef);
    REQUIRE(index.Version() == 7);
    REQUIRE(index.NextVersion(19).Value() == 19);
    REQUIRE(VersionedIndex::FromRepresentation(~std::uint32_t{0}).Representation() == ~std::uint32_t{0});

    using SparseIndex = Sora::Concurrency::VersionedUnsigned<std::uint32_t, 20, 8>;
    REQUIRE(SparseIndex::FromRepresentation(~std::uint32_t{0}).Representation() == 0x0fff'ffff);

    using TaggedState = Sora::Concurrency::TaggedValue<SlotStateCodec>;
    constexpr TaggedState state{SlotState::Ready, 9};
    STATIC_REQUIRE(state.Value() == SlotState::Ready);
    STATIC_REQUIRE(state.Tag() == 9);

    Sora::Concurrency::AtomicStampedValue atomic{versioned};
    auto expected = atomic.Load();
    REQUIRE(atomic.CompareExchangeStrong(expected, versioned.NextVersion()));
    REQUIRE(atomic.Load().Version() == 0);
    STATIC_REQUIRE(decltype(atomic)::IsAlwaysLockFree());
    STATIC_REQUIRE(sizeof(atomic) == sizeof(std::uintptr_t));
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<decltype(atomic)>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::HazardDomain::HazardPointer>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::EpochDomain::Participant>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::QsbrDomain::Participant>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::RcuDomain::Participant>);
}

TEST_CASE("Hazard pointer owns one protection record in Release and may outlive its creator scope",
          "[Sora.Core.Concurrency.Reclamation][Hazard]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::HazardDomain domain;
    std::atomic<Node*> source{new Node{7}};
    auto hazard = domain.MakeHazardPointer();
    REQUIRE(hazard.Protect(source)->value == 7);

    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    REQUIRE(domain.Collect() == 0);
    hazard.ResetProtection();
    REQUIRE(domain.Collect() == 1);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Hazard collection invokes custom deleters outside retirement locks",
          "[Sora.Core.Concurrency.Reclamation][Hazard][Reentrant]") {
    Sora::Concurrency::HazardDomain domain;
    std::atomic<int> reclaimed = 0;
    domain.Retire(new int{1}, [&](int* value) noexcept {
        delete value;
        reclaimed.fetch_add(1, std::memory_order_relaxed);
        domain.Retire(new int{2});
    });
    REQUIRE(domain.Collect() == 1);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 1);
    REQUIRE(domain.RetiredCount() == 1);
    REQUIRE(domain.Collect() == 1);
}

TEST_CASE("Hazard protection preserves a stamped pointer's exact publication identity",
          "[Sora.Core.Concurrency.Reclamation][Hazard][Stamp]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::HazardDomain domain;
    using Pointer = Sora::Concurrency::VersionedPointer<Node, 4>;
    Sora::Concurrency::AtomicStampedValue source{Pointer{new Node{9}, 3}};

    auto protectedValue = domain.Protect(source);
    REQUIRE(protectedValue->value == 9);
    REQUIRE(protectedValue.Snapshot().Version() == 3);
    auto moved = std::move(protectedValue);
    REQUIRE_FALSE(protectedValue);
    protectedValue = std::move(moved);
    REQUIRE_FALSE(moved);
    const Pointer removed = source.Exchange(protectedValue.Snapshot().NextVersion(nullptr), std::memory_order_seq_cst);
    domain.Retire(removed.Value());
    REQUIRE(domain.Collect() == 0);
    protectedValue = {};
    REQUIRE(domain.Collect() == 1);
}

TEST_CASE("Epoch and RCU protection preserve stamped publication identity",
          "[Sora.Core.Concurrency.Reclamation][Epoch][RCU][Stamp]") {
    Node node{23};
    using Pointer = Sora::Concurrency::VersionedPointer<Node, 4>;
    Sora::Concurrency::AtomicStampedValue source{Pointer{&node, 6}};

    Sora::Concurrency::EpochDomain epoch;
    auto epochParticipant = epoch.Register();
    auto epochObservation = epochParticipant.Protect(source);
    REQUIRE(epochObservation.Get() == &node);
    REQUIRE(epochObservation.Snapshot().Version() == 6);

    Sora::Concurrency::RcuDomain rcu;
    auto rcuParticipant = rcu.Register();
    auto rcuObservation = rcuParticipant.Protect(source);
    REQUIRE(rcuObservation.Get() == &node);
    REQUIRE(rcuObservation.Snapshot().Version() == 6);
}

TEST_CASE("Hazard protect-unlink-retire-collect stress never reclaims a protected object",
          "[Sora.Core.Concurrency.Reclamation][Hazard][Stress]") {
    constexpr std::size_t iterations = 20'000;
    Sora::Concurrency::HazardDomain domain;
    std::atomic<Node*> source{new Node{31}};
    std::atomic<bool> stop = false;
    std::atomic<std::size_t> observations = 0;
    std::atomic<bool> invalidObservation = false;
    std::barrier start{2};
    std::jthread reader([&] {
        auto hazard = domain.MakeHazardPointer();
        start.arrive_and_wait();
        while (!stop.load(std::memory_order_acquire)) {
            if (Node* node = hazard.Protect(source); node != nullptr) {
                if (node->value != 31) {
                    invalidObservation.store(true, std::memory_order_relaxed);
                }
                observations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    start.arrive_and_wait();
    while (observations.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
        (void)domain.Collect();
        source.store(new Node{31}, std::memory_order_seq_cst);
    }
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    stop.store(true, std::memory_order_release);
    reader.join();
    while (domain.RetiredCount() != 0) {
        (void)domain.Collect();
    }
    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE(observations.load(std::memory_order_relaxed) != 0);
}

TEST_CASE("Epoch readers require two completed transitions after retirement",
          "[Sora.Core.Concurrency.Reclamation][Epoch]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::EpochDomain domain;
    auto participant = domain.Register();
    auto outer = participant.Enter();
    auto inner = participant.Enter();
    domain.Retire(new Node{11});
    REQUIRE(domain.TryAdvance());
    REQUIRE_FALSE(domain.TryAdvance());
    REQUIRE(domain.Collect() == 0);
    inner.Reset();
    outer.Reset();
    REQUIRE(domain.TryAdvance());
    REQUIRE(domain.Collect() == 1);
}

TEST_CASE("Epoch guard delays reuse after its participant handle is destroyed",
          "[Sora.Core.Concurrency.Reclamation][Epoch][Lifetime]") {
    Sora::Concurrency::EpochDomain domain;
    Sora::Concurrency::EpochDomain::ReadGuard guard;
    {
        auto participant = domain.Register();
        guard = participant.Enter();
    }
    auto replacement = domain.Register();
    (void)replacement;
    REQUIRE(domain.TryAdvance());
    REQUIRE_FALSE(domain.TryAdvance());
    guard.Reset();
    REQUIRE(domain.TryAdvance());
}

TEST_CASE("RCU guard keeps its record leased after its participant handle is destroyed",
          "[Sora.Core.Concurrency.Reclamation][RCU][Lifetime]") {
    Sora::Concurrency::RcuDomain domain;
    Sora::Concurrency::RcuDomain::ReadGuard oldGuard;
    {
        auto participant = domain.Register();
        oldGuard = participant.Read();
    }

    domain.Retire(new int{1});
    std::atomic<std::size_t> reclaimed = 0;
    std::jthread reclaimer([&] { reclaimed.store(domain.Reclaim(), std::memory_order_release); });
    while (domain.RetiredCount() != 0) {
        std::this_thread::yield();
    }

    auto replacement = domain.Register();
    (void)replacement;
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 0);
    oldGuard.Reset();
    reclaimer.join();
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 1);
}

TEST_CASE("QSBR collection waits for every registered participant to acknowledge a quiescent state",
          "[Sora.Core.Concurrency.Reclamation][QSBR]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::QsbrDomain domain;
    std::barrier entered{2};
    std::atomic<bool> report = false;
    std::jthread participant([&] {
        auto registration = domain.Register();
        entered.arrive_and_wait();
        while (!report.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        registration.Quiescent();
    });
    entered.arrive_and_wait();
    domain.Retire(new Node{13});
    (void)domain.Advance();
    REQUIRE(domain.Collect() == 0);
    report.store(true, std::memory_order_release);
    participant.join();
    REQUIRE(domain.Collect() == 1);
}

TEST_CASE("RCU grace period waits for an old reader while new read regions continue",
          "[Sora.Core.Concurrency.Reclamation][RCU]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::RcuDomain domain;
    std::latch oldEntered{1};
    std::atomic<bool> releaseOld = false;
    std::jthread oldReader([&] {
        auto participant = domain.Register();
        auto guard = participant.Read();
        oldEntered.count_down();
        while (!releaseOld.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    oldEntered.wait();
    domain.Retire(new Node{17});

    std::atomic<std::size_t> reclaimed = 0;
    std::jthread synchronizer([&] { reclaimed.store(domain.Reclaim(), std::memory_order_release); });
    while (domain.RetiredCount() != 0) {
        std::this_thread::yield();
    }

    std::atomic<bool> churn = true;
    std::jthread newReaders([&] {
        auto participant = domain.Register();
        while (churn.load(std::memory_order_acquire)) {
            auto guard = participant.Read();
        }
    });
    releaseOld.store(true, std::memory_order_release);
    oldReader.join();
    synchronizer.join();
    churn.store(false, std::memory_order_release);
    newReaders.join();
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 1);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Reclamation domains drain uncollected ownership at destruction",
          "[Sora.Core.Concurrency.Reclamation][Lifetime]") {
    CheckDomainDrainsRetiredObjects<Sora::Concurrency::HazardDomain>();
    CheckDomainDrainsRetiredObjects<Sora::Concurrency::EpochDomain>();
    CheckDomainDrainsRetiredObjects<Sora::Concurrency::QsbrDomain>();
    CheckDomainDrainsRetiredObjects<Sora::Concurrency::RcuDomain>();
}

TEST_CASE("Reclamation domains batch-retire non-null pointers exactly once",
          "[Sora.Core.Concurrency.Reclamation][Batch]") {
    CheckBatchRetirementConsumesEachNonNullPointerOnce<Sora::Concurrency::HazardDomain>();
    CheckBatchRetirementConsumesEachNonNullPointerOnce<Sora::Concurrency::EpochDomain>();
    CheckBatchRetirementConsumesEachNonNullPointerOnce<Sora::Concurrency::QsbrDomain>();
    CheckBatchRetirementConsumesEachNonNullPointerOnce<Sora::Concurrency::RcuDomain>();
}

TEST_CASE("Batch retirement invokes reentrant deleters outside its lock",
          "[Sora.Core.Concurrency.Reclamation][Batch][Reentrant]") {
    Sora::Concurrency::HazardDomain domain;
    std::atomic<std::size_t> reclaimed = 0;
    std::array pointers{new int{1}, new int{2}};
    domain.Retire(std::span<int* const, pointers.size()>{pointers}, [&](int* pointer) noexcept {
        delete pointer;
        reclaimed.fetch_add(1, std::memory_order_relaxed);
        domain.Retire(new int{3});
    });
    REQUIRE(domain.Collect() == 2);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 2);
    REQUIRE(domain.RetiredCount() == 2);
    REQUIRE(domain.Collect() == 2);
}

TEST_CASE("Domain destruction drains objects retired by a reentrant deleter",
          "[Sora.Core.Concurrency.Reclamation][Lifetime][Reentrant]") {
    Node::destroyed.store(0, std::memory_order_relaxed);
    {
        Sora::Concurrency::HazardDomain domain;
        domain.Retire(new Node{1}, [&](Node* node) noexcept {
            delete node;
            domain.Retire(new Node{2});
        });
    }
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == 2);
}
