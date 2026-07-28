#include "Sora/Core/Resources/Resources.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace R = Sora::Resources;

namespace Sora::Demos::Resources::Settings::Render {

    inline constexpr unsigned char Pipeline[] = {
#embed "Assets/RenderSettings.json"
    };

} // namespace Sora::Demos::Resources::Settings::Render

namespace {

    using RenderPipelineSettings =
        R::StaticSetting<R::ComposeResourceDomainUri<R::SettingsDomain, Sora::FixedString<256>{"render"},
                                                     Sora::FixedString<256>{"pipeline"}>(),
                         Sora::Demos::Resources::Settings::Render::Pipeline>;

    template<typename T>
    concept RenderPipelineSettingsShape = requires(T settings) {
        settings.render.backend;
        settings.render.width;
        settings.render.height;
        settings.render.vsync;
        settings.render.shader;
        settings.render.scales;
        requires std::is_integral_v<decltype(settings.render.width)>;
        requires std::is_same_v<decltype(settings.render.vsync), bool>;
    };

    static_assert(RenderPipelineSettingsShape<RenderPipelineSettings::Value>);
    static_assert(RenderPipelineSettings::kSettingsUri == "settings://render/pipeline");
    static_assert(RenderPipelineSettings::kResourceUri == "res://settings/render/pipeline");
    static_assert(RenderPipelineSettings::kId.kHash == R::HashUri("res://settings/render/pipeline"));
    static_assert(std::string_view(RenderPipelineSettings::kValue.render.backend) == "vulkan");
    static_assert(RenderPipelineSettings::kValue.render.width == 1920);
    static_assert(RenderPipelineSettings::kValue.render.height == 1080);
    static_assert(RenderPipelineSettings::kValue.render.vsync);
    static_assert(std::string_view(RenderPipelineSettings::kValue.render.shader) == "res://shader/demo.wgsl");
    static_assert(RenderPipelineSettings::kValue.render.scales.size() == 3);
    static_assert(RenderPipelineSettings::kValue.render.scales[2] == 0.5);

    [[nodiscard]] auto Text(std::span<const std::byte> bytes) noexcept -> std::string_view {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    template<typename T>
        requires(!std::is_void_v<T>)
    [[nodiscard]] auto Require(Sora::Result<T>&& result) -> T {
        if (!result) {
            std::println(stderr, "Settings demo failed with ErrorCode 0x{:04X}.",
                         static_cast<unsigned>(result.error()));
        }
        assert(result.has_value());
        return std::move(*result);
    }

    void Require(Sora::VoidResult&& result) {
        if (!result) {
            std::println(stderr, "Settings demo failed with ErrorCode 0x{:04X}.",
                         static_cast<unsigned>(result.error()));
        }
        assert(result.has_value());
    }

    void CompileTimeSettingsDemo() {
        constexpr const auto& value = RenderPipelineSettings::kValue;
        static_assert(value.render.width * value.render.height == 2'073'600);
        static_assert(R::IsResourceDomainUri<R::SettingsDomain>("settings://render/pipeline?profile=debug"));
        static_assert(R::IsResourceDomainUri<R::SettingsDomain>("res://settings/render/pipeline"));
        static_assert(!R::IsResourceDomainUri<R::SettingsDomain>("res://shader/demo.wgsl"));
    }

    void StaticResourceSettingsDemo() {
        constexpr auto table = R::StaticSparseTable<RenderPipelineSettings>::Table();
        static_assert(table.count == 1);
        static_assert(table.entries[0].semanticHash == R::HashUri("res://settings/render/pipeline"));
        static_assert(table.entries[0].type == static_cast<uint16_t>(R::ResourceType::Settings));

        R::ResourceRegistry registry;
        Require(registry.AddSparse(table, {}));

        auto byAlias = Require(registry.Open("settings://render/pipeline"));
        auto byCanonical = Require(registry.Open("res://settings/render/pipeline"));

        assert(byAlias.Id().type == R::ResourceType::Settings);
        assert(byAlias.Id().uri == "res://settings/render/pipeline");
        assert(byCanonical.Id().uri == byAlias.Id().uri);
        assert(byCanonical.Bytes().data() == byAlias.Bytes().data());
        assert(Text(byAlias.Bytes()).contains("\"backend\": \"vulkan\""));
    }

    void RuntimeOverrideSettingsDemo() {
        constexpr auto table = R::StaticSparseTable<RenderPipelineSettings>::Table();

        R::RuntimeSettingsProvider runtimeSettings;
        Require(runtimeSettings.DeclareOverride<RenderPipelineSettings>());
        Require(runtimeSettings.SetText("settings://render/pipeline",
                                        R"({"render":{"backend":"webgpu","width":1280,"height":720}})"));

        R::ResourceRegistry registry;
        Require(registry.AddSparse(table, {}));
        auto provider = runtimeSettings.ProviderDescriptor(100);
        Require(registry.AddProvider(provider, {}));

        auto active = Require(registry.Open("settings://render/pipeline"));
        assert(active.Id().uri == "res://settings/render/pipeline");
        assert(Text(active.Bytes()).contains("\"backend\":\"webgpu\""));

        Require(runtimeSettings.SetText("res://settings/render/pipeline",
                                        R"({"render":{"backend":"vulkan","width":2560,"height":1440}})"));
        auto updated = Require(registry.Open("settings://render/pipeline"));
        assert(Text(updated.Bytes()).contains("\"width\":2560"));
    }

} // namespace

int main() {
    CompileTimeSettingsDemo();
    StaticResourceSettingsDemo();
    RuntimeOverrideSettingsDemo();

    std::println("Settings demo passed: compile-time JSON value, settings:// alias, res://settings resource, and "
                 "runtime override verified.");
    return 0;
}
