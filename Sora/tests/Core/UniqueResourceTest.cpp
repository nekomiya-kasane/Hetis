#include <Sora/Core/UniqueResource.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <stdexcept>
#include <utility>

namespace {

    struct RecordingDeleter {
        int* calls;
        int* lastValue;

        void operator()(int& value) const noexcept {
            ++*calls;
            *lastValue = value;
        }
    };

    struct SourceDeleter {
        int* calls;

        void operator()(int&) const noexcept { ++*calls; }
    };

    struct ThrowingStoredDeleter {
        explicit ThrowingStoredDeleter(SourceDeleter&) { throw std::runtime_error{"deleter copy failed"}; }
        void operator()(int&) const noexcept {}
    };

    struct DeleterProxy {};

    struct DeleterFromProxy {
        constexpr explicit DeleterFromProxy(DeleterProxy) noexcept {}
        constexpr void operator()(int&) const noexcept {}
    };

    struct SourceResource {
        int value;
    };

    struct ThrowingResource {
        explicit ThrowingResource(SourceResource&) { throw std::runtime_error{"resource copy failed"}; }
    };

    struct ResourceRollbackDeleter {
        int* calls;

        void operator()(SourceResource&) const noexcept { ++*calls; }
        void operator()(ThrowingResource&) const noexcept { ++*calls; }
    };

    struct ThrowingTransferDeleter {
        int* calls;
        bool* throwOnCopy;

        ThrowingTransferDeleter(int& callCount, bool& shouldThrow) noexcept
            : calls{&callCount}, throwOnCopy{&shouldThrow} {}

        ThrowingTransferDeleter(const ThrowingTransferDeleter& other)
            : calls{other.calls}, throwOnCopy{other.throwOnCopy} {
            if (*throwOnCopy) {
                throw std::runtime_error{"transfer failed"};
            }
        }

        ThrowingTransferDeleter(ThrowingTransferDeleter&& other) noexcept(false) : ThrowingTransferDeleter{other} {}

        void operator()(int&) const noexcept { ++*calls; }
    };

    struct Node {
        int value;
    };

} // namespace

static_assert(std::constructible_from<Sora::UniqueResource<int, DeleterFromProxy>, int, DeleterProxy>);
static_assert(Sora::Concept::SourcePreservingConstructible<DeleterFromProxy, DeleterProxy>);
static_assert(Sora::Concept::NothrowForwardConstructible<DeleterFromProxy, DeleterProxy>);

TEST_CASE("UniqueResource reset, release, and move preserve single ownership", "[Sora][Core][UniqueResource]") {
    int calls = 0;
    int lastValue = 0;
    RecordingDeleter deleter{&calls, &lastValue};

    {
        Sora::UniqueResource first{7, deleter};
        REQUIRE(first.Get() == 7);
        first.Reset(9);
        REQUIRE(calls == 1);
        REQUIRE(lastValue == 7);

        Sora::UniqueResource second{std::move(first)};
        REQUIRE(second.Get() == 9);
        second.Release();
    }
    REQUIRE(calls == 1);

    {
        Sora::UniqueResource source{11, deleter};
        Sora::UniqueResource destination{13, deleter};
        destination = std::move(source);
        REQUIRE(calls == 2);
        REQUIRE(lastValue == 13);
    }
    REQUIRE(calls == 3);
    REQUIRE(lastValue == 11);
}

TEST_CASE("UniqueResource rolls back partial construction", "[Sora][Core][UniqueResource]") {
    SECTION("deleter storage fails") {
        int calls = 0;
        SourceDeleter deleter{&calls};

        REQUIRE_THROWS_AS((Sora::UniqueResource<int, ThrowingStoredDeleter>{17, deleter}), std::runtime_error);
        REQUIRE(calls == 1);
    }

    SECTION("resource storage fails") {
        int calls = 0;
        SourceResource resource{17};
        ResourceRollbackDeleter deleter{&calls};

        REQUIRE_THROWS_AS((Sora::UniqueResource<ThrowingResource, ResourceRollbackDeleter>{resource, deleter}),
                          std::runtime_error);
        REQUIRE(calls == 1);
    }
}

TEST_CASE("UniqueResource move failure consumes and releases a moved resource", "[Sora][Core][UniqueResource]") {
    int calls = 0;
    bool throwOnCopy = false;
    ThrowingTransferDeleter deleter{calls, throwOnCopy};
    Sora::UniqueResource<int, ThrowingTransferDeleter> source{23, deleter};

    throwOnCopy = true;
    REQUIRE_THROWS_AS((Sora::UniqueResource<int, ThrowingTransferDeleter>{std::move(source)}), std::runtime_error);
    REQUIRE(calls == 1);
}

TEST_CASE("UniqueResource supports reference and pointer resources", "[Sora][Core][UniqueResource]") {
    int calls = 0;
    int value = 5;
    {
        Sora::UniqueResource<int&, RecordingDeleter> referenceOwner{value, RecordingDeleter{&calls, &value}};
        referenceOwner.Get() = 8;
        REQUIRE(value == 8);
    }
    REQUIRE(calls == 1);

    Node node{.value = 31};
    bool deleted = false;
    {
        Sora::UniqueResource pointerOwner{&node, [&](Node*) noexcept { deleted = true; }};
        REQUIRE(pointerOwner->value == 31);
        (*pointerOwner).value = 37;
        REQUIRE(node.value == 37);
    }
    REQUIRE(deleted);
}

TEST_CASE("MakeUniqueResourceChecked disarms only the invalid sentinel", "[Sora][Core][UniqueResource]") {
    int calls = 0;
    int lastValue = 0;
    RecordingDeleter deleter{&calls, &lastValue};
    {
        auto invalid = Sora::MakeUniqueResourceChecked(-1, -1, deleter);
        auto valid = Sora::MakeUniqueResourceChecked(41, -1, deleter);
        REQUIRE(invalid.Get() == -1);
        REQUIRE(valid.Get() == 41);
    }
    REQUIRE(calls == 1);
    REQUIRE(lastValue == 41);
}
