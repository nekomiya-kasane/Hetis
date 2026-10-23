#include "Sora/Core/RefCounted.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    struct Tracked : Sora::RefCounted<Tracked> {
        Tracked() noexcept { ++live; }
        ~Tracked() { --live; }

        static inline int live = 0;

        int payload = 0;
    };

    /** @brief Counted type that keeps its constructor and destructor private. */
    class Encapsulated final : public Sora::RefCounted<Encapsulated> {
    public:
        [[nodiscard]] static Sora::IntrusivePtr<Encapsulated> Create(int value) {
            return Sora::MakeRefCounted<Encapsulated>(value);
        }

        [[nodiscard]] int Value() const noexcept { return value_; }

        static inline int live = 0;

    private:
        friend struct Sora::Detail::MakeRefCountedHelper<Encapsulated>;
        friend class Sora::RefCounted<Encapsulated>;

        explicit Encapsulated(int value) noexcept : value_(value) { ++live; }

        ~Encapsulated() { --live; }

        int value_ = 0;
    };

} // namespace

TEST_CASE("RefCounted owns its count and destroys with the last reference", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> owner = Sora::MakeRefCounted<Tracked>();
        REQUIRE(owner);
        REQUIRE(Tracked::live == 1);
        CHECK(owner->HasOneRef());

        {
            Sora::IntrusivePtr<Tracked> copy = owner;
            CHECK_FALSE(owner->HasOneRef());
            CHECK(copy.Get() == owner.Get());
            CHECK(Tracked::live == 1);
        }

        CHECK(owner->HasOneRef());
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("IntrusivePtr moves transfer the owned reference", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> source = Sora::MakeRefCounted<Tracked>();
        Tracked* raw = source.Get();

        Sora::IntrusivePtr<Tracked> moved = std::move(source);
        CHECK_FALSE(source);
        CHECK(moved.Get() == raw);
        CHECK(moved->HasOneRef());

        Sora::IntrusivePtr<Tracked> assigned;
        assigned = std::move(moved);
        CHECK_FALSE(moved);
        CHECK(assigned.Get() == raw);
        CHECK(Tracked::live == 1);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("IntrusivePtr resets retain before release", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> first = Sora::MakeRefCounted<Tracked>();
        Sora::IntrusivePtr<Tracked> second = Sora::MakeRefCounted<Tracked>();
        REQUIRE(Tracked::live == 2);

        first.Reset(second.Get());
        CHECK(Tracked::live == 1);
        CHECK(first.Get() == second.Get());
        CHECK_FALSE(second->HasOneRef());

        first.Reset(first.Get());
        CHECK_FALSE(second->HasOneRef());

        first.Reset();
        CHECK(second->HasOneRef());
        CHECK(Tracked::live == 1);

        second = nullptr;
        CHECK(Tracked::live == 0);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("IntrusivePtr adopts constructor references and converts to const", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> adopted = Sora::AdoptRef(new Tracked());
        CHECK(adopted->HasOneRef());

        adopted->payload = 11;
        CHECK((*adopted).payload == 11);

        Sora::IntrusivePtr<const Tracked> readOnly = adopted;
        CHECK(readOnly.Get() == adopted.Get());
        CHECK_FALSE(adopted->HasOneRef());

        Sora::IntrusivePtr<Tracked> borrowed{adopted.Get()};
        CHECK_FALSE(adopted->HasOneRef());
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("IntrusivePtr swaps without touching the count", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> left = Sora::MakeRefCounted<Tracked>();
        Sora::IntrusivePtr<Tracked> right = Sora::MakeRefCounted<Tracked>();
        Tracked* leftRaw = left.Get();
        Tracked* rightRaw = right.Get();

        left.Swap(right);
        CHECK(left.Get() == rightRaw);
        CHECK(right.Get() == leftRaw);
        CHECK(left->HasOneRef());
        CHECK(right->HasOneRef());

        Sora::Swap(left, right);
        CHECK(left.Get() == leftRaw);
        CHECK(right.Get() == rightRaw);

        Sora::swap(left, right);
        CHECK(left.Get() == rightRaw);
        CHECK(right.Get() == leftRaw);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("MakeRefCounted reaches private constructors through the helper", "[Sora.Core.RefCounted]") {
    Encapsulated::live = 0;
    {
        Sora::IntrusivePtr<Encapsulated> value = Encapsulated::Create(7);
        CHECK(value->Value() == 7);
        CHECK(Encapsulated::live == 1);
        CHECK(value->HasOneRef());
    }
    CHECK(Encapsulated::live == 0);
}

TEST_CASE("RefCounted is atomic across concurrent copies and releases", "[Sora.Core.RefCounted]") {
    Tracked::live = 0;
    {
        Sora::IntrusivePtr<Tracked> owner = Sora::MakeRefCounted<Tracked>();

        std::vector<std::thread> workers;
        workers.reserve(4);
        for (int index = 0; index < 4; ++index) {
            workers.emplace_back([shared = owner] {
                for (int iteration = 0; iteration < 2000; ++iteration) {
                    Sora::IntrusivePtr<Tracked> local = shared;
                    local.Reset();
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }

        CHECK(owner->HasOneRef());
        CHECK(Tracked::live == 1);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("RefCounted and IntrusivePtr expose the expected type properties", "[Sora.Core.RefCounted]") {
    STATIC_REQUIRE(sizeof(Sora::IntrusivePtr<Tracked>) == sizeof(Tracked*));
    STATIC_REQUIRE(alignof(Sora::IntrusivePtr<Tracked>) == alignof(Tracked*));
    STATIC_REQUIRE(!std::is_copy_constructible_v<Tracked>);
    STATIC_REQUIRE(!std::is_copy_assignable_v<Tracked>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Sora::IntrusivePtr<Tracked>>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Sora::IntrusivePtr<Tracked>>);
    STATIC_REQUIRE(!std::is_trivially_copyable_v<Sora::IntrusivePtr<Tracked>>);

    STATIC_REQUIRE(Sora::Concept::RefCountedObject<Tracked>);
    STATIC_REQUIRE(Sora::Concept::RefCountedObject<const Tracked>);
    STATIC_REQUIRE_FALSE(Sora::Concept::RefCountedObject<int>);
    STATIC_REQUIRE_FALSE(Sora::Concept::RefCountedObject<Tracked*>);
}
