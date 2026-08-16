/**
 * @file ReclamationTest.cpp
 * @brief Verify stamped publication and safe-reclamation lifetime protocols.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/StampedValue.h>
#include <Sora/Core/Concurrency/Reclamation.h>
#include <Sora/Core/InlineFunction.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

    struct alignas(16) Node {
        int value = 0;
        inline static std::atomic<int> destroyed = 0;
        ~Node() { destroyed.fetch_add(1, std::memory_order_relaxed); }
    };

    struct LinkNode {
        int value = 0;
        LinkNode* next = nullptr;
        LinkNode* alternate = nullptr;
    };

    struct PoolNode {
        PoolNode* next = nullptr;
    };

    struct NonDefaultPayload {
        explicit NonDefaultPayload(int value) : value(value) {}
        NonDefaultPayload(const NonDefaultPayload&) = delete;
        NonDefaultPayload& operator=(const NonDefaultPayload&) = delete;
        NonDefaultPayload(NonDefaultPayload&&) noexcept = default;
        NonDefaultPayload& operator=(NonDefaultPayload&&) noexcept = default;

        int value;
    };

    struct ThrowingPayload {
        explicit ThrowingPayload(bool shouldThrow) {
            if (shouldThrow) {
                throw std::runtime_error("injected value construction failure");
            }
        }
    };

    struct ThrowingDestructible {
        ~ThrowingDestructible() noexcept(false) {}
    };

    template<typename T>
    struct FailingAllocator {
        using value_type = T;

        template<typename>
        friend struct FailingAllocator;

        FailingAllocator() noexcept = default;
        explicit FailingAllocator(std::atomic<bool>& shouldFail) noexcept : shouldFail(&shouldFail) {}

        template<typename U>
        FailingAllocator(const FailingAllocator<U>& other) noexcept : shouldFail(other.shouldFail) {}

        [[nodiscard]] T* allocate(std::size_t count) {
            if (shouldFail != nullptr && shouldFail->load(std::memory_order_relaxed)) {
                throw std::bad_alloc{};
            }
            return std::allocator<T>{}.allocate(count);
        }

        void deallocate(T* pointer, std::size_t count) noexcept { std::allocator<T>{}.deallocate(pointer, count); }

        template<typename U>
        [[nodiscard]] bool operator==(const FailingAllocator<U>& other) const noexcept {
            return shouldFail == other.shouldFail;
        }

        template<typename U>
        [[nodiscard]] bool operator!=(const FailingAllocator<U>& other) const noexcept {
            return !(*this == other);
        }

        std::atomic<bool>* shouldFail = nullptr;
    };

    template<typename Domain, typename T>
    concept DefaultRetirable = requires(Domain& domain, T* pointer) { domain.Retire(pointer); };

    template<typename Domain, typename T>
    concept DefaultBatchRetirable = requires(Domain& domain, std::span<T* const> pointers) { domain.Retire(pointers); };

    using BatchQueue = Sora::Concurrency::ConcurrentBatchQueue<NonDefaultPayload, 2>;
    static_assert(std::same_as<decltype(std::declval<BatchQueue&>().Emplace(1)), void>);

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

TEST_CASE("Intrusive topology supports independent links and detached segments",
          "[Sora.Core.ADT][IntrusiveForwardList]") {
    std::array<LinkNode, 3> nodes{{{1}, {2}, {3}}};
    Sora::IntrusiveForwardList<LinkNode, &LinkNode::next> primary;
    Sora::IntrusiveForwardList<LinkNode, &LinkNode::alternate> secondary;

    primary.PushBack(&nodes[0]);
    primary.PushBack(&nodes[1]);
    secondary.PushBack(&nodes[1]);
    secondary.PushBack(&nodes[2]);

    auto detached = primary.DetachAll();
    REQUIRE(primary.Empty());
    REQUIRE(detached.Size() == 2);
    REQUIRE(detached.Head() == &nodes[0]);
    REQUIRE(detached.Tail() == &nodes[1]);
    primary.Append(std::move(detached));
    REQUIRE(primary.PopFront() == &nodes[0]);
    REQUIRE(primary.PopFront() == &nodes[1]);
    REQUIRE(primary.Empty());
    REQUIRE(secondary.PopFront() == &nodes[1]);
    REQUIRE(secondary.PopFront() == &nodes[2]);
    REQUIRE(secondary.Empty());
}

TEST_CASE("Paged node pool grows by pages and reuses released addresses", "[Sora.Core.ADT][PagedNodePool]") {
    Sora::PagedNodePool<PoolNode, &PoolNode::next, 2> pool;
    PoolNode* first = pool.Acquire();
    PoolNode* second = pool.Acquire();
    REQUIRE(pool.PageCount() == 1);
    REQUIRE(pool.Capacity() == 2);

    PoolNode* third = pool.Acquire();
    REQUIRE(pool.PageCount() == 2);
    REQUIRE(pool.Capacity() == 4);

    pool.Release(first);
    REQUIRE(pool.Acquire() == first);
    pool.Release(second);
    pool.Release(third);
}

TEST_CASE("Concurrent batch queue supports non-default values and batch recycling",
          "[Sora.Core.Concurrency][ConcurrentBatchQueue]") {
    Sora::Concurrency::ConcurrentBatchQueue<NonDefaultPayload, 2> queue;
    queue.Emplace(1);
    const std::array inputs{2, 3, 4};
    queue.EmplaceBatch(inputs, [](int value) -> std::optional<NonDefaultPayload> {
        if (value == 3) {
            return std::nullopt;
        }
        return std::optional<NonDefaultPayload>{std::in_place, value};
    });

    REQUIRE(queue.Size() == 3);
    REQUIRE(queue.PageCount() == 2);
    auto batch = queue.TakeAll();
    REQUIRE(queue.Size() == 0);
    auto partition = batch.MakeEmptySibling();
    auto* first = batch.PopFront();
    REQUIRE(first != nullptr);
    REQUIRE(Sora::Concurrency::ConcurrentBatchQueue<NonDefaultPayload, 2>::Batch::Value(*first).value == 1);
    partition.PushBack(first);
    batch.Append(std::move(partition));
    queue.Requeue(std::move(batch));

    auto released = queue.TakeAll();
    int sum = 0;
    while (auto* entry = released.PopFront()) {
        sum += Sora::Concurrency::ConcurrentBatchQueue<NonDefaultPayload, 2>::Batch::Value(*entry).value;
    }
    REQUIRE(sum == 7);
    queue.Release(std::move(released));
    REQUIRE(queue.Size() == 0);
}

TEST_CASE("Concurrent batch queue recycles an entry when single construction fails",
          "[Sora.Core.Concurrency][ConcurrentBatchQueue][Exception]") {
    Sora::Concurrency::ConcurrentBatchQueue<ThrowingPayload, 1> queue;
    REQUIRE_THROWS_AS(queue.Emplace(true), std::runtime_error);
    REQUIRE(queue.Size() == 0);
    REQUIRE(queue.PageCount() == 1);

    queue.Emplace(false);
    REQUIRE(queue.Size() == 1);
    REQUIRE(queue.PageCount() == 1);
    queue.Release(queue.TakeAll());
}

TEST_CASE("Retired objects reclaim ownership when batch staging fails",
          "[Sora.Core.Concurrency.Reclamation][Batch][Exception]") {
    using RetiredObject = Sora::Concurrency::Detail::RetiredObject;
    Sora::Concurrency::ConcurrentBatchQueue<RetiredObject, 2> queue;
    std::atomic<std::size_t> reclaimed = 0;
    std::atomic<bool> observedUnlockedQueue = false;
    const std::array inputs{1, 2};

    REQUIRE_THROWS_AS(queue.EmplaceBatch(inputs,
                                         [&](int value) {
                                             if (value == 2) {
                                                 throw std::runtime_error("injected batch staging failure");
                                             }
                                             Sora::InlineFunction<void() noexcept, 32> callback{
                                                 [&queue, &reclaimed, &observedUnlockedQueue]() noexcept {
                                                     reclaimed.fetch_add(1, std::memory_order_relaxed);
                                                     observedUnlockedQueue.store(queue.Size() == 0,
                                                                                 std::memory_order_relaxed);
                                                 }};
                                             return RetiredObject{nullptr, std::move(callback), 0};
                                         }),
                      std::runtime_error);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 0);
    REQUIRE_FALSE(observedUnlockedQueue.load(std::memory_order_relaxed));
    REQUIRE(queue.Size() == 1);

    queue.Release(queue.TakeAll());
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 1);
    REQUIRE(observedUnlockedQueue.load(std::memory_order_relaxed));
    REQUIRE(queue.Size() == 0);
}

TEST_CASE("Retirement allocation failure preserves pointer ownership until retry",
          "[Sora.Core.Concurrency.Reclamation][Exception][Allocation]") {
    using RetiredObject = Sora::Concurrency::Detail::RetiredObject;
    using Allocator = FailingAllocator<RetiredObject>;
    using RetirementQueue = Sora::Concurrency::Detail::BasicRetirementQueue<Allocator>;

    std::atomic<bool> shouldFail = true;
    std::atomic<bool> protectedReader = true;
    std::atomic<std::size_t> reclaimed = 0;
    std::atomic<bool> prematureReclaim = false;
    Allocator allocator{shouldFail};
    RetirementQueue queue{allocator};
    int* pointer = new int{1};
    auto deleter = [&](int* value) noexcept {
        if (protectedReader.load(std::memory_order_relaxed)) {
            prematureReclaim.store(true, std::memory_order_relaxed);
        }
        delete value;
        reclaimed.fetch_add(1, std::memory_order_relaxed);
    };

    REQUIRE_THROWS_AS(queue.Retire(pointer, deleter), std::bad_alloc);
    REQUIRE(queue.Size() == 0);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 0);
    REQUIRE_FALSE(prematureReclaim.load(std::memory_order_relaxed));

    shouldFail.store(false, std::memory_order_relaxed);
    queue.Retire(pointer, deleter);
    REQUIRE(queue.Size() == 1);
    protectedReader.store(false, std::memory_order_relaxed);
    queue.Destroy(queue.DetachAll());
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 1);
    REQUIRE_FALSE(prematureReclaim.load(std::memory_order_relaxed));
}

TEST_CASE("Batch retirement allocation failure preserves every pointer until retry",
          "[Sora.Core.Concurrency.Reclamation][Batch][Exception][Allocation]") {
    using RetiredObject = Sora::Concurrency::Detail::RetiredObject;
    using Allocator = FailingAllocator<RetiredObject>;
    using RetirementQueue = Sora::Concurrency::Detail::BasicRetirementQueue<Allocator>;

    std::atomic<bool> shouldFail = true;
    std::atomic<bool> protectedReader = true;
    std::atomic<std::size_t> reclaimed = 0;
    std::atomic<bool> prematureReclaim = false;
    Allocator allocator{shouldFail};
    RetirementQueue queue{allocator};
    std::array pointers{new int{1}, new int{2}, static_cast<int*>(nullptr), new int{3}};
    auto deleter = [&](int* value) noexcept {
        if (protectedReader.load(std::memory_order_relaxed)) {
            prematureReclaim.store(true, std::memory_order_relaxed);
        }
        delete value;
        reclaimed.fetch_add(1, std::memory_order_relaxed);
    };

    REQUIRE_THROWS_AS(queue.Retire(std::span{pointers}, deleter), std::bad_alloc);
    REQUIRE(queue.Size() == 0);
    REQUIRE(pointers[0] != nullptr);
    REQUIRE(pointers[1] != nullptr);
    REQUIRE(pointers[3] != nullptr);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 0);
    REQUIRE_FALSE(prematureReclaim.load(std::memory_order_relaxed));

    shouldFail.store(false, std::memory_order_relaxed);
    queue.Retire(std::span{pointers}, deleter);
    REQUIRE(queue.Size() == 3);
    protectedReader.store(false, std::memory_order_relaxed);
    queue.Destroy(queue.DetachAll());
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 3);
    REQUIRE_FALSE(prematureReclaim.load(std::memory_order_relaxed));
}

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
    expected = versioned;
    REQUIRE_FALSE(atomic.CompareExchangeStrong(expected, versioned.NextVersion()));
    REQUIRE(expected == atomic.Load());
    STATIC_REQUIRE(decltype(atomic)::IsAlwaysLockFree());
    STATIC_REQUIRE(sizeof(atomic) == sizeof(std::uintptr_t));
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<decltype(atomic)>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::HazardDomain::HazardPointer>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::EpochDomain::Participant>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::QsbrDomain::Participant>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sora::Concurrency::RcuDomain::Participant>);
}

TEST_CASE("Default retirement rejects potentially throwing pointee destructors",
          "[Sora.Core.Concurrency.Reclamation][Contract]") {
    STATIC_REQUIRE_FALSE(DefaultRetirable<Sora::Concurrency::HazardDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultRetirable<Sora::Concurrency::EpochDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultRetirable<Sora::Concurrency::QsbrDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultRetirable<Sora::Concurrency::RcuDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultBatchRetirable<Sora::Concurrency::HazardDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultBatchRetirable<Sora::Concurrency::EpochDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultBatchRetirable<Sora::Concurrency::QsbrDomain, ThrowingDestructible>);
    STATIC_REQUIRE_FALSE(DefaultBatchRetirable<Sora::Concurrency::RcuDomain, ThrowingDestructible>);
}

TEST_CASE("Reclamation generation ordering handles wraparound within its half-range contract",
          "[Sora.Core.Concurrency.Reclamation][Generation]") {
    using Sora::Concurrency::Detail::GenerationReached;
    constexpr std::uint64_t maximum = ~std::uint64_t{0};
    constexpr std::uint64_t halfRange = Sora::Concurrency::Detail::kGenerationHalfRange;

    STATIC_REQUIRE(GenerationReached(0, 0));
    STATIC_REQUIRE(GenerationReached(0, maximum));
    STATIC_REQUIRE_FALSE(GenerationReached(maximum, 0));
    STATIC_REQUIRE(GenerationReached(0, halfRange + 1));
    STATIC_REQUIRE_FALSE(GenerationReached(0, halfRange));
    STATIC_REQUIRE_FALSE(GenerationReached(halfRange, 0));
    STATIC_REQUIRE(GenerationReached(halfRange - 1, 0));
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

TEST_CASE("Hazard collection scans records across page boundaries",
          "[Sora.Core.Concurrency.Reclamation][Hazard][Registry]") {
    constexpr std::size_t recordCount = 17;
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::HazardDomain domain;
    std::atomic<Node*> source{new Node{8}};
    std::vector<Sora::Concurrency::HazardDomain::HazardPointer> hazards;
    hazards.reserve(recordCount);
    for (std::size_t index = 0; index < recordCount; ++index) {
        hazards.push_back(domain.MakeHazardPointer());
        REQUIRE(hazards.back().Protect(source)->value == 8);
    }

    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    REQUIRE(domain.Collect() == 0);
    for (auto& hazard : hazards) {
        hazard.ResetProtection();
    }
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

TEST_CASE("Hazard collection scratch remains valid across recursive collection",
          "[Sora.Core.Concurrency.Reclamation][Hazard][Reentrant]") {
    Sora::Concurrency::HazardDomain domain;
    std::atomic<std::size_t> nestedCollection = 0;
    std::atomic<std::size_t> reclaimed = 0;

    domain.Retire(new int{1}, [&](int* outer) noexcept {
        delete outer;
        reclaimed.fetch_add(1, std::memory_order_relaxed);
        domain.Retire(new int{2}, [&](int* inner) noexcept {
            delete inner;
            reclaimed.fetch_add(1, std::memory_order_relaxed);
        });
        nestedCollection.store(domain.Collect(), std::memory_order_relaxed);
    });

    REQUIRE(domain.Collect() == 1);
    REQUIRE(nestedCollection.load(std::memory_order_relaxed) == 1);
    REQUIRE(reclaimed.load(std::memory_order_relaxed) == 2);
    REQUIRE(domain.RetiredCount() == 0);
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

TEST_CASE("Epoch protect-unlink-retire-collect stress preserves active observations",
          "[Sora.Core.Concurrency.Reclamation][Epoch][Stress]") {
    constexpr std::size_t iterations = 2'000;
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::EpochDomain domain;
    std::atomic<Node*> source{new Node{37}};
    std::atomic<bool> stop = false;
    std::atomic<bool> invalidObservation = false;
    std::atomic<std::size_t> observations = 0;
    std::barrier start{2};
    std::jthread reader([&] {
        auto participant = domain.Register();
        start.arrive_and_wait();
        while (!stop.load(std::memory_order_acquire)) {
            if (auto protectedNode = participant.Protect(source); protectedNode && protectedNode->value != 37) {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
            observations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    start.arrive_and_wait();

    std::size_t reclaimed = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
        while (!domain.TryAdvance()) {
            std::this_thread::yield();
        }
        while (!domain.TryAdvance()) {
            std::this_thread::yield();
        }
        reclaimed += domain.Collect();
        source.store(new Node{37}, std::memory_order_seq_cst);
    }
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    while (!domain.TryAdvance()) {
        std::this_thread::yield();
    }
    while (!domain.TryAdvance()) {
        std::this_thread::yield();
    }
    reclaimed += domain.Collect();
    stop.store(true, std::memory_order_release);
    reader.join();

    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE(observations.load(std::memory_order_relaxed) != 0);
    REQUIRE(reclaimed == iterations + 1);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == static_cast<int>(iterations + 1));
}

TEST_CASE("QSBR unlink-retire-synchronize stress waits for post-read quiescence",
          "[Sora.Core.Concurrency.Reclamation][QSBR][Stress]") {
    constexpr std::size_t iterations = 2'000;
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::QsbrDomain domain;
    std::atomic<Node*> source{new Node{41}};
    std::atomic<bool> stop = false;
    std::atomic<bool> invalidObservation = false;
    std::atomic<std::size_t> observations = 0;
    std::barrier start{2};
    std::jthread reader([&] {
        auto participant = domain.Register();
        start.arrive_and_wait();
        while (!stop.load(std::memory_order_acquire)) {
            if (Node* node = source.load(std::memory_order_acquire); node != nullptr && node->value != 41) {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
            participant.Quiescent();
            observations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    start.arrive_and_wait();

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
        domain.Synchronize();
        source.store(new Node{41}, std::memory_order_seq_cst);
    }
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    domain.Synchronize();
    stop.store(true, std::memory_order_release);
    reader.join();

    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE(observations.load(std::memory_order_relaxed) != 0);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == static_cast<int>(iterations + 1));
}

TEST_CASE("RCU protect-unlink-retire-reclaim stress preserves old-phase observations",
          "[Sora.Core.Concurrency.Reclamation][RCU][Stress]") {
    constexpr std::size_t iterations = 2'000;
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::RcuDomain domain;
    std::atomic<Node*> source{new Node{43}};
    std::atomic<bool> stop = false;
    std::atomic<bool> invalidObservation = false;
    std::atomic<std::size_t> observations = 0;
    std::barrier start{2};
    std::jthread reader([&] {
        auto participant = domain.Register();
        start.arrive_and_wait();
        while (!stop.load(std::memory_order_acquire)) {
            if (auto protectedNode = participant.Protect(source); protectedNode && protectedNode->value != 43) {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
            observations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    start.arrive_and_wait();

    std::size_t reclaimed = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
        reclaimed += domain.Reclaim();
        source.store(new Node{43}, std::memory_order_seq_cst);
    }
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    reclaimed += domain.Reclaim();
    stop.store(true, std::memory_order_release);
    reader.join();

    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE(observations.load(std::memory_order_relaxed) != 0);
    REQUIRE(reclaimed == iterations + 1);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == static_cast<int>(iterations + 1));
}

TEST_CASE("RCU registration churn remains safe while collection scans records",
          "[Sora.Core.Concurrency.Reclamation][RCU][Registration][Stress]") {
    constexpr std::size_t iterations = 2'000;
    Node::destroyed.store(0, std::memory_order_relaxed);
    Sora::Concurrency::RcuDomain domain;
    std::atomic<Node*> source{new Node{47}};
    std::atomic<bool> stop = false;
    std::atomic<bool> invalidObservation = false;
    std::atomic<std::size_t> registrations = 0;
    std::barrier start{2};
    std::jthread reader([&] {
        start.arrive_and_wait();
        while (!stop.load(std::memory_order_acquire)) {
            auto participant = domain.Register();
            if (auto protectedNode = participant.Protect(source); protectedNode && protectedNode->value != 47) {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
            registrations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    start.arrive_and_wait();

    std::size_t reclaimed = 0;
    for (std::size_t iteration = 0; iteration != iterations; ++iteration) {
        domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
        reclaimed += domain.Reclaim();
        source.store(new Node{47}, std::memory_order_seq_cst);
    }
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));
    reclaimed += domain.Reclaim();
    stop.store(true, std::memory_order_release);
    reader.join();

    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE(registrations.load(std::memory_order_relaxed) != 0);
    REQUIRE(reclaimed == iterations + 1);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == static_cast<int>(iterations + 1));
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

    for (std::size_t iteration = 0; iteration != 4'096; ++iteration) {
        auto replacement = domain.Register();
        auto replacementGuard = replacement.Read();
    }
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 0);
    oldGuard.Reset();
    reclaimer.join();
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 1);
}

TEST_CASE("RCU empty reclaim does not enter a grace period", "[Sora.Core.Concurrency.Reclamation][RCU][FastPath]") {
    Sora::Concurrency::RcuDomain domain;
    auto participant = domain.Register();
    auto guard = participant.Read();
    REQUIRE(domain.Reclaim() == 0);
}

TEST_CASE("RCU nested guards retain one old-phase interval until the final exit",
          "[Sora.Core.Concurrency.Reclamation][RCU][Nested]") {
    Sora::Concurrency::RcuDomain domain;
    auto participant = domain.Register();
    auto outer = participant.Read();
    auto inner = participant.Read();
    domain.Retire(new int{1});

    std::atomic<std::size_t> reclaimed = 0;
    std::jthread reclaimer([&] { reclaimed.store(domain.Reclaim(), std::memory_order_release); });
    while (domain.RetiredCount() != 0) {
        std::this_thread::yield();
    }

    outer.Reset();
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 0);
    inner.Reset();
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
    std::atomic<Node*> source{new Node{17}};
    std::atomic<bool> invalidObservation = false;
    std::atomic<bool> invalidNewObservation = false;
    std::jthread oldReader([&] {
        auto participant = domain.Register();
        auto protectedNode = participant.Protect(source);
        oldEntered.count_down();
        while (!releaseOld.load(std::memory_order_acquire)) {
            if (protectedNode->value != 17 || Node::destroyed.load(std::memory_order_relaxed) != 0) {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });
    oldEntered.wait();
    domain.Retire(source.exchange(nullptr, std::memory_order_seq_cst));

    std::atomic<std::size_t> reclaimed = 0;
    std::jthread synchronizer([&] { reclaimed.store(domain.Reclaim(), std::memory_order_release); });
    while (domain.RetiredCount() != 0) {
        std::this_thread::yield();
    }

    std::atomic<bool> churn = true;
    std::jthread newReaders([&] {
        auto participant = domain.Register();
        while (churn.load(std::memory_order_acquire)) {
            if (participant.Protect(source)) {
                invalidNewObservation.store(true, std::memory_order_relaxed);
            }
        }
    });
    REQUIRE(reclaimed.load(std::memory_order_acquire) == 0);
    REQUIRE(Node::destroyed.load(std::memory_order_relaxed) == 0);
    releaseOld.store(true, std::memory_order_release);
    oldReader.join();
    synchronizer.join();
    churn.store(false, std::memory_order_release);
    newReaders.join();
    REQUIRE_FALSE(invalidObservation.load(std::memory_order_relaxed));
    REQUIRE_FALSE(invalidNewObservation.load(std::memory_order_relaxed));
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
