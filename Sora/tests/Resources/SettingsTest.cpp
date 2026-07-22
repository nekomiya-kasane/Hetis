#include <Sora/Core/Resources/Resources.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace Sora::Literals;
using namespace Sora::Resources;

namespace {

    [[nodiscard]] auto Text(std::span<const std::byte> bytes) -> std::string_view {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    constexpr const unsigned char kRenderSettingsJson[] = {
#embed "SettingsTest.json"
    };

    using RenderPipelineSettings = StaticSettingFor<"render"_FS, "pipeline"_FS, kRenderSettingsJson>;

    template<typename T>
    concept RenderSettingsShape = requires(T settings) {
        settings.render.width;
        settings.render.height;
        settings.render.vsync;
        settings.render.shader;
        settings.render.scales;
        requires std::is_integral_v<decltype(settings.render.width)>;
        requires std::is_same_v<decltype(settings.render.vsync), bool>;
    };

    static_assert(RenderSettingsShape<RenderPipelineSettings::Value>);
    static_assert(RenderPipelineSettings::kId.kHash == HashUri("res://settings/render/pipeline"));
    static_assert(RenderPipelineSettings::kResourceUri == "res://settings/render/pipeline");
    static_assert(RenderPipelineSettings::kValue.render.width == 1920);
    static_assert(RenderPipelineSettings::kValue.render.height == 1080);
    static_assert(RenderPipelineSettings::kValue.render.vsync);
    static_assert(std::string_view(RenderPipelineSettings::kValue.render.shader) == "res://shader/demo.wgsl");
    static_assert(RenderPipelineSettings::kValue.render.scales.size() == 2);
    static_assert(RenderPipelineSettings::kValue.render.scales[1] == 0.5);

} // namespace

TEST_CASE("StaticSetting parses embedded JSON and exposes canonical settings resources", "[Sora][Resources]") {
    constexpr auto table = StaticSparseTable<RenderPipelineSettings>::Table();
    REQUIRE(table.count == 1);

    const ModuleResourceEntry& entry = table.entries[0];
    REQUIRE(static_cast<ResourceType>(entry.type) == ResourceType::Settings);
    REQUIRE(entry.semanticHash == HashUri("res://settings/render/pipeline"));
    REQUIRE(std::string_view{entry.uri, entry.uriSize} == "res://settings/render/pipeline");

    ResourceRegistry registry;
    REQUIRE(registry.AddSparse(table, {}).has_value());

    auto byAlias = registry.Open("settings://render/pipeline");
    REQUIRE(byAlias.has_value());
    REQUIRE(byAlias->Id().type == ResourceType::Settings);
    REQUIRE(byAlias->Id().uri == "res://settings/render/pipeline");
    REQUIRE(Text(byAlias->Bytes()).starts_with("{\n  \"render\""));
    REQUIRE(!Text(byAlias->Bytes()).ends_with('\0'));

    auto byResourceUri = registry.Open("res://settings/render/pipeline");
    REQUIRE(byResourceUri.has_value());
    REQUIRE(byResourceUri->Bytes().data() == byAlias->Bytes().data());
}

TEST_CASE("RuntimeSettingsProvider updates settings without reparsing static resource paths", "[Sora][Resources]") {
    RuntimeSettingsProvider provider;
    REQUIRE(provider.SetText("settings://render/pipeline", R"({"quality":"low"})").has_value());

    ResourceRegistry registry;
    auto descriptor = provider.Provider(10);
    REQUIRE(registry.AddProvider(descriptor, {}).has_value());

    auto first = registry.Open("settings://render/pipeline");
    REQUIRE(first.has_value());
    REQUIRE(first->Id().uri == "res://settings/render/pipeline");
    REQUIRE(Text(first->Bytes()) == R"({"quality":"low"})");

    REQUIRE(provider.SetText("settings://render/pipeline", R"({"quality":"high"})").has_value());
    auto updated = registry.Open("settings://render/pipeline");
    REQUIRE(updated.has_value());
    REQUIRE(Text(updated->Bytes()) == R"({"quality":"high"})");
}

TEST_CASE("settings URI helpers reject non-canonical runtime identity paths", "[Sora][Resources]") {
    static_assert(IsSettingsUri("settings://module/entry"));
    static_assert(!IsSettingsUri("settings://module/../entry"));
    static_assert(!IsSettingsUri("settings://module/entry?debug=true"));

    REQUIRE(NormalizeResourceOrSettingsUri("settings://module/entry").value() == "res://settings/module/entry");
    REQUIRE_FALSE(NormalizeResourceOrSettingsUri("settings://module/../entry").has_value());
}