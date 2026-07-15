#include <Sora/Core/Environment.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ConfigEnvironment = Sora::Configuration;
namespace PAL = Sora::PAL;

namespace {

    struct Database {
        std::string host = "default-host";
        std::uint16_t port = 1000;
    };

    struct RuntimeFlags {
        bool enabled = false;
    };

    struct RuntimeCache {
        int generation = 0;
    };

    struct [[= ConfigEnvironment::$::Object{.prefix = "SORA_TEST_BINDING"}]] ApplicationEnvironment {
        Database database;

        [[= ConfigEnvironment::$::Path{.value = "runtime.workerCount"}]] std::uint32_t workers = 4;

        [[= ConfigEnvironment::$::Alias{.value = "legacy.token"},
          = ConfigEnvironment::$::Required{}]] std::string token;

        [[= ConfigEnvironment::$::NativeName{.value = "SORA_TEST_BINDING_EXACT"}]] std::optional<std::string> exact;

        [[= ConfigEnvironment::$::Flatten{}]] RuntimeFlags flags;

        [[= ConfigEnvironment::$::Ignore{}]] RuntimeCache cache;
    };

    inline constexpr auto kEnvironment = ConfigEnvironment::Compile<ApplicationEnvironment>();
    static_assert(decltype(kEnvironment)::kFieldCount == 6);

    class RestoreVariables {
    public:
        RestoreVariables() {
            for (std::string_view name : Names()) {
                auto original = PAL::ReadEnvironmentVariable(name);
                if (!original) {
                    throw std::runtime_error("failed to preserve an environment variable for the test");
                }
                originals_.push_back(std::move(*original));
            }
        }

        ~RestoreVariables() {
            for (const auto& [name, original] : std::ranges::zip_view(Names(), originals_)) {
                if (original) {
                    std::ignore = PAL::WriteEnvironmentVariable(name, *original);
                } else {
                    std::ignore = PAL::RemoveEnvironmentVariable(name);
                }
            }
        }

        [[nodiscard]] static constexpr std::array<std::string_view, 7> Names() {
            return {"SORA_TEST_BINDING__DATABASE__HOST",
                    "SORA_TEST_BINDING__DATABASE__PORT",
                    "SORA_TEST_BINDING__RUNTIME__WORKER_COUNT",
                    "SORA_TEST_BINDING__TOKEN",
                    "SORA_TEST_BINDING__LEGACY__TOKEN",
                    "SORA_TEST_BINDING_EXACT",
                    "SORA_TEST_BINDING__ENABLED"};
        }

    private:
        std::vector<std::optional<std::string>> originals_;
    };

} // namespace

TEST_CASE("Hierarchical environment scope maps dotted paths without literal dots", "[Sora.Core.Environment]") {
    const auto scope = ConfigEnvironment::Scope::Create("SoraTest");
    REQUIRE(scope.has_value());
    const auto replicaHost = scope->NativeNameOf("database.readReplica.host");
    REQUIRE(replicaHost.has_value());
    REQUIRE(*replicaHost == "SORA_TEST__DATABASE__READ_REPLICA__HOST");

    const auto database = scope->Child("database");
    REQUIRE(database.has_value());
    const auto databaseHost = database->NativeNameOf("host");
    REQUIRE(databaseHost.has_value());
    REQUIRE(*databaseHost == "SORA_TEST__DATABASE__HOST");
}

TEST_CASE("Compiled environment reads nested fields, aliases, optional values, and defaults",
          "[Sora.Core.Environment]") {
    RestoreVariables restore;
    for (std::string_view name : RestoreVariables::Names()) {
        REQUIRE(PAL::RemoveEnvironmentVariable(name).has_value());
    }

    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING__DATABASE__HOST", "db.internal").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING__DATABASE__PORT", "5432").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING__RUNTIME__WORKER_COUNT", "32").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING__LEGACY__TOKEN", "secret").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING_EXACT", "exact-value").has_value());
    REQUIRE(PAL::WriteEnvironmentVariable("SORA_TEST_BINDING__ENABLED", "yes").has_value());

    const auto loaded = kEnvironment.Read();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->database.host == "db.internal");
    REQUIRE(loaded->database.port == 5432);
    REQUIRE(loaded->workers == 32);
    REQUIRE(loaded->token == "secret");
    REQUIRE(loaded->exact == "exact-value");
    REQUIRE(loaded->flags.enabled);
    REQUIRE(loaded->cache.generation == 0);
}

TEST_CASE("Compiled environment reports missing required values and writes transactional patches",
          "[Sora.Core.Environment]") {
    RestoreVariables restore;
    for (std::string_view name : RestoreVariables::Names()) {
        REQUIRE(PAL::RemoveEnvironmentVariable(name).has_value());
    }

    const auto missing = kEnvironment.Read();
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().issues.size() == 1);
    REQUIRE(missing.error().issues.front().kind == ConfigEnvironment::IssueKind::MissingRequired);

    ApplicationEnvironment object;
    object.database.host = "written-host";
    object.database.port = 6543;
    object.workers = 12;
    object.token = "written-token";
    object.exact.reset();
    object.flags.enabled = true;

    const auto patch = kEnvironment.Encode(object);
    REQUIRE(patch.Size() == decltype(kEnvironment)::kFieldCount);
    REQUIRE(patch.Apply().has_value());
    const auto writtenHost = PAL::ReadEnvironmentVariable("SORA_TEST_BINDING__DATABASE__HOST");
    REQUIRE(writtenHost.has_value());
    REQUIRE(writtenHost->has_value());
    REQUIRE(**writtenHost == "written-host");
    const auto exact = PAL::ReadEnvironmentVariable("SORA_TEST_BINDING_EXACT");
    REQUIRE(exact.has_value());
    REQUIRE_FALSE(exact->has_value());
}
