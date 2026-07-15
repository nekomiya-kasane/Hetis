#include <Sora/Core/PAL/Environment.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

namespace PAL = Sora::PAL;

namespace {

    class RestoreVariable {
    public:
        explicit RestoreVariable(std::string name)
            : name_{std::move(name)}, original_{PAL::ReadEnvironmentVariable(name_)} {}

        ~RestoreVariable() {
            if (!original_) {
                return;
            }
            if (*original_) {
                static_cast<void>(PAL::WriteEnvironmentVariable(name_, **original_));
            } else {
                static_cast<void>(PAL::RemoveEnvironmentVariable(name_));
            }
        }

    private:
        std::string name_;
        Sora::Result<std::optional<std::string>> original_;
    };

} // namespace

TEST_CASE("PAL environment distinguishes missing, empty, and populated values", "[Sora.PAL.Environment]") {
    constexpr std::string_view name = "SORA_TEST__PAL_ENVIRONMENT__VALUE";
    RestoreVariable restore{std::string{name}};

    REQUIRE(PAL::RemoveEnvironmentVariable(name).has_value());
    const auto missing = PAL::ReadEnvironmentVariable(name);
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->has_value());

    REQUIRE(PAL::WriteEnvironmentVariable(name, "").has_value());
    const auto empty = PAL::ReadEnvironmentVariable(name);
    REQUIRE(empty.has_value());
    REQUIRE(empty->has_value());
    REQUIRE(empty->value().empty());

    REQUIRE(PAL::WriteEnvironmentVariable(name, "\xE4\xB8\xAD\xE6\x96\x87").has_value());
    const auto populated = PAL::ReadEnvironmentVariable(name);
    REQUIRE(populated.has_value());
    REQUIRE(populated->has_value());
    REQUIRE(**populated == "\xE4\xB8\xAD\xE6\x96\x87");
}

TEST_CASE("PAL environment snapshots support sorted point and prefix lookup", "[Sora.PAL.Environment]") {
    constexpr std::string_view first = "SORA_TEST__PAL_ENVIRONMENT__TREE__FIRST";
    constexpr std::string_view second = "SORA_TEST__PAL_ENVIRONMENT__TREE__SECOND";
    RestoreVariable restoreFirst{std::string{first}};
    RestoreVariable restoreSecond{std::string{second}};

    REQUIRE(PAL::WriteEnvironmentVariable(first, "1").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable(second, "2").has_value());
    const auto snapshot = PAL::CaptureEnvironment();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->Find(first) == std::optional<std::string_view>{"1"});
    REQUIRE(snapshot->PrefixRange("SORA_TEST__PAL_ENVIRONMENT__TREE__").Size() >= 2);
}

TEST_CASE("PAL environment rejects malformed and duplicate mutations before applying", "[Sora.PAL.Environment]") {
    REQUIRE(PAL::WriteEnvironmentVariable("", "value").error() == Sora::ErrorCode::InvalidEnvironmentName);
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST__PAL_ENVIRONMENT__INVALID", "\xF0\x28\x8C\x28").error() ==
            Sora::ErrorCode::InvalidEnvironmentValue);

    constexpr std::array duplicate{
        PAL::EnvironmentMutation{.name = "SORA_TEST__PAL_ENVIRONMENT__DUPLICATE", .value = "1"},
        PAL::EnvironmentMutation{.name = "SORA_TEST__PAL_ENVIRONMENT__DUPLICATE", .value = "2"},
    };
    const auto applied = PAL::ApplyEnvironmentMutations(duplicate);
    REQUIRE_FALSE(applied.has_value());
    REQUIRE(applied.error() == Sora::ErrorCode::DuplicateEnvironmentMutation);
}

TEST_CASE("PAL environment name ordering follows platform semantics", "[Sora.PAL.Environment]") {
    STATIC_REQUIRE(PAL::CompareEnvironmentNames("ALPHA", "BETA") < 0);
    STATIC_REQUIRE(PAL::CompareEnvironmentNames("BETA", "ALPHA") > 0);
    STATIC_REQUIRE(PAL::CompareEnvironmentNames("ALPHA", "ALPHA") == 0);
#ifdef PLATFORM_WINDOWS
    STATIC_REQUIRE(PAL::CompareEnvironmentNames("alpha", "ALPHA") == 0);
#else
    STATIC_REQUIRE(PAL::CompareEnvironmentNames("alpha", "ALPHA") > 0);
#endif
}
