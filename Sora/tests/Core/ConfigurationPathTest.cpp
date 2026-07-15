#include <Sora/Core/Configuration/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Configuration paths validate dotted hierarchy", "[Sora.Core.Configuration.Path]") {
    STATIC_REQUIRE(Sora::Configuration::ValidatePath("database.readReplica.host").has_value());
    STATIC_REQUIRE(Sora::Configuration::ValidatePath("").error() == Sora::ErrorCode::EmptyConfigurationPath);
    STATIC_REQUIRE(Sora::Configuration::ValidatePath("database..host").error() ==
                   Sora::ErrorCode::EmptyConfigurationPathSegment);
    STATIC_REQUIRE(Sora::Configuration::ValidatePath("database/host").error() ==
                   Sora::ErrorCode::InvalidConfigurationPathCharacter);
    STATIC_REQUIRE(Sora::Configuration::PathView::Parse("database.host")->Depth() == 2);
}

TEST_CASE("Configuration paths encode portable hierarchical environment names", "[Sora.Core.Configuration.Path]") {
    STATIC_REQUIRE(Sora::Ascii::ToUpperSnake("HTTPServer2Path") == "HTTP_SERVER2_PATH");
    STATIC_REQUIRE(Sora::Configuration::EncodeEnvironmentName("SoraApp", "database.readReplica.host") ==
                   std::string{"SORA_APP__DATABASE__READ_REPLICA__HOST"});
    STATIC_REQUIRE(Sora::Configuration::JoinPath("database", "primary.host") == std::string{"database.primary.host"});
}
