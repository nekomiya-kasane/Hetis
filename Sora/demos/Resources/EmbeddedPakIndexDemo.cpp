#include "Sora/Core/Resources/Resources.h"

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace R = Sora::Resources;

#ifndef SORA_DEMO_EMBEDDED_PAK_MODULE_NAME
#    if defined(_WIN32)
#        define SORA_DEMO_EMBEDDED_PAK_MODULE_NAME "Sora.Resources.EmbeddedPakModule.dll"
#    elif defined(__APPLE__)
#        define SORA_DEMO_EMBEDDED_PAK_MODULE_NAME "libSora.Resources.EmbeddedPakModule.dylib"
#    else
#        define SORA_DEMO_EMBEDDED_PAK_MODULE_NAME "libSora.Resources.EmbeddedPakModule.so"
#    endif
#endif

namespace {

    template<typename T>
    [[nodiscard]] auto Require(Sora::Result<T>&& result) -> T {
        if (!result) {
            std::println(stderr, "Sora resource demo failed with ErrorCode 0x{:04X}.",
                         static_cast<unsigned>(result.error()));
        }
        assert(result.has_value());
        return std::move(*result);
    }

    void Require(const Sora::VoidResult& result) {
        if (!result) {
            std::println(stderr, "Sora resource demo failed with ErrorCode 0x{:04X}.",
                         static_cast<unsigned>(result.error()));
        }
        assert(result.has_value());
    }

    [[nodiscard]] auto DefaultModulePath(char** argv) -> std::filesystem::path {
        auto executable = std::filesystem::absolute(std::filesystem::path{argv[0]});
        return executable.parent_path() / SORA_DEMO_EMBEDDED_PAK_MODULE_NAME;
    }

    [[nodiscard]] auto ModulePath(int argc, char** argv) -> std::filesystem::path {
        if (argc > 1 && std::string_view{argv[1]}.size() != 0) {
            return argv[1];
        }
        return DefaultModulePath(argv);
    }

    [[nodiscard]] auto Text(std::span<const std::byte> bytes) -> std::string_view {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

} // namespace

int main(int argc, char** argv) {
    auto& registry = R::ResourceRegistry::Default();
    const auto modulePath = std::filesystem::absolute(ModulePath(argc, argv));
    registry.AddModuleCandidate(modulePath.string());

    auto shader = Require(registry.Open("res://shader/demo.wgsl"));
    auto material = Require(registry.Open("res://material/demo.json"));
    auto scene = Require(registry.Open("res://scene/demo.json"));
    auto mesh = Require(registry.Open("res://model/demo.mesh"));
    auto config = Require(registry.Open("res://config/render.ini"));

    assert(registry.ModuleCount() == 1);
    assert(registry.LayoutCount() == 2);
    assert(registry.ResourceCount() == 5);
    assert(shader.Id().type == R::ResourceType::Shader);
    assert(material.Id().type == R::ResourceType::Material);
    assert(scene.Id().type == R::ResourceType::Scene);
    assert(mesh.Id().type == R::ResourceType::Model);
    assert(config.Id().type == R::ResourceType::Config);
    assert(Text(shader.Bytes()).contains("@fragment fn fs_main"));
    assert(Text(material.Bytes()).contains("embedded-demo-material"));
    assert(Text(scene.Bytes()).contains("res://shader/demo.wgsl"));
    assert(Text(mesh.Bytes()).contains("vertices"));
    assert(Text(config.Bytes()).contains("renderer.backend=vulkan"));

    const auto missing = registry.Open("res://missing/not-in-module.asset");
    assert(!missing && missing.error() == Sora::ErrorCode::ResourceNotFound);

    std::println("Indexed {} visible resources from {} layouts and {} module in {}", registry.ResourceCount(),
                 registry.LayoutCount(), registry.ModuleCount(), modulePath.string());
    return 0;
}
