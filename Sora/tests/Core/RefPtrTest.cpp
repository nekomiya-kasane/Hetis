#include "Sora/Core/RefPtr.h"
#include "Sora/Core/Hash.h"
#include "Sora/Core/RefPtrJson.h"
#include "Sora/Core/ToJson.h"
#include "Sora/Core/ToString.h"

#include <catch2/catch_test_macros.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace {

    struct Base {
        int value = 1;
    };

    struct Derived : Base {
        int extra = 2;
    };

    template<typename T>
    concept HasDeref = requires(T pointer) {
        *pointer;
    };

    template<typename T>
    concept HasArrow = requires(T pointer) {
        pointer.operator->();
    };

    [[nodiscard]] constexpr bool ConstexprRefPtrWorks() {
        int value = 42;
        Sora::RefPtr<int> pointer{&value};
        Sora::RefPtr<int> copy = pointer;
        copy.Reset(&value);
        return pointer && copy == pointer && *copy == 42;
    }

} // namespace

TEST_CASE("RefPtr preserves raw pointer representation and type traits", "[Sora.Core.RefPtr]") {
    STATIC_REQUIRE(sizeof(Sora::RefPtr<int>) == sizeof(int*));
    STATIC_REQUIRE(alignof(Sora::RefPtr<int>) == alignof(int*));
    STATIC_REQUIRE(std::is_trivially_copyable_v<Sora::RefPtr<int>>);
    STATIC_REQUIRE(std::is_standard_layout_v<Sora::RefPtr<int>>);
    STATIC_REQUIRE(sizeof(Sora::RefPtr<void>) == sizeof(void*));

    STATIC_REQUIRE(Sora::Traits::RefPtrType<Sora::RefPtr<int>>);
    STATIC_REQUIRE(Sora::Traits::RefPtrType<const Sora::RefPtr<int>&>);
    STATIC_REQUIRE_FALSE(Sora::Traits::RefPtrType<int*>);
    STATIC_REQUIRE(std::same_as<Sora::Traits::RefPtrElement<Sora::RefPtr<const int>>, const int>);

    STATIC_REQUIRE(!HasDeref<Sora::RefPtr<void>>);
    STATIC_REQUIRE(!HasArrow<Sora::RefPtr<void>>);
    STATIC_REQUIRE(ConstexprRefPtrWorks());
}

TEST_CASE("RefPtr construction, conversion, and observer semantics", "[Sora.Core.RefPtr]") {
    int value = 7;
    const Sora::RefPtr<int> pointer{&value};

    REQUIRE(pointer);
    REQUIRE(pointer.Get() == &value);
    REQUIRE(static_cast<int*>(pointer) == &value);
    STATIC_REQUIRE(std::same_as<decltype(*pointer), int&>);

    *pointer = 11;
    REQUIRE(value == 11);

    Derived derived{};
    Sora::RefPtr<Derived> derivedPointer{&derived};
    Sora::RefPtr<Base> basePointer = derivedPointer;
    REQUIRE(basePointer.Get() == static_cast<Base*>(&derived));

    auto fromObject = Sora::Ref(value);
    REQUIRE(fromObject.Get() == &value);

    int* raw = &value;
    auto fromPointer = Sora::Ref(raw);
    REQUIRE(fromPointer.Get() == &value);

    auto made = Sora::MakeRef(&value);
    REQUIRE(made.Get() == &value);
}

TEST_CASE("RefPtr mutation, swap, and identity comparison", "[Sora.Core.RefPtr]") {
    int a = 1;
    int b = 2;
    Sora::RefPtr<int> lhs{&a};
    Sora::RefPtr<int> rhs{&b};

    REQUIRE(lhs == &a);
    REQUIRE(lhs != rhs);
    REQUIRE((lhs <=> rhs) != std::strong_ordering::equal);

    lhs.Reset(&b);
    REQUIRE(lhs == rhs);

    lhs.Reset();
    REQUIRE(lhs == nullptr);
    REQUIRE(!lhs);

    lhs.Reset(&a);
    Sora::Swap(lhs, rhs);
    REQUIRE(lhs.Get() == &b);
    REQUIRE(rhs.Get() == &a);

    swap(lhs, rhs);
    REQUIRE(lhs.Get() == &a);
    REQUIRE(rhs.Get() == &b);

    std::set<Sora::RefPtr<int>> ordered;
    ordered.insert(lhs);
    ordered.insert(rhs);
    REQUIRE(ordered.size() == 2);

    std::map<Sora::RefPtr<int>, int> mapped;
    mapped.emplace(lhs, 1);
    mapped.emplace(rhs, 2);
    REQUIRE(mapped.at(lhs) == 1);
    REQUIRE(mapped.at(rhs) == 2);
}

TEST_CASE("RefPtr integrates with Sora Hash, ToString, and ToJson by address identity", "[Sora.Core.RefPtr]") {
    int value = 5;
    Sora::RefPtr<int> pointer{&value};
    Sora::RefPtr<int> same{&value};
    Sora::RefPtr<int> empty{};

    REQUIRE(Sora::Hashing::Hash(pointer) == Sora::Hashing::Hash(same));
    REQUIRE(Sora::Hashing::Hash(empty) == Sora::Hashing::Hash(Sora::RefPtr<int>{}));

    REQUIRE(Sora::ToString(empty) == "null");
    const std::string text = Sora::ToString(pointer);
    REQUIRE(text.find("RefPtr<") == 0);
    REQUIRE(text.find('(') != std::string::npos);
    REQUIRE(text.find(')') != std::string::npos);

    const Sora::json emptyJson = Sora::ToJson(empty);
    REQUIRE(emptyJson.is_null());

    const Sora::json pointerJson = Sora::ToJson(pointer);
    REQUIRE(pointerJson.is_string());
    REQUIRE(!pointerJson.get<std::string>().empty());
}
