#include "Sora/Core/Resources/Resources.h"

#include <cstdint>

namespace R = Sora::Resources;

namespace Sora::Demos::Resources::EmbeddedPak::Model {

    inline constexpr unsigned char Demo[] = {
#embed "Assets/Demo.mesh"
    };

} // namespace Sora::Demos::Resources::EmbeddedPak::Model

namespace Sora::Demos::Resources::EmbeddedPak::Material {

    inline constexpr unsigned char Demo[] = {
#embed "Assets/DemoMaterial.json"
    };

} // namespace Sora::Demos::Resources::EmbeddedPak::Material

namespace Sora::Demos::Resources::EmbeddedPak::Shader {

    inline constexpr unsigned char Demo[] = {
#embed "Assets/DemoShader.wgsl"
    };

} // namespace Sora::Demos::Resources::EmbeddedPak::Shader

namespace Sora::Demos::Resources::EmbeddedPak::Scene {

    inline constexpr unsigned char Demo[] = {
#embed "Assets/DemoScene.json"
    };

} // namespace Sora::Demos::Resources::EmbeddedPak::Scene

namespace Sora::Demos::Resources::EmbeddedPak::Config {

    inline constexpr unsigned char Render[] = {
#embed "Assets/Render.ini"
    };

} // namespace Sora::Demos::Resources::EmbeddedPak::Config

namespace {

    using ResourceList = R::ResourceListOf<^^Sora::Demos::Resources::EmbeddedPak>;
    using PakImage = R::StaticPakImage<ResourceList>;
    using SparseResources = R::StaticSparseTableFor<ResourceList>;

    inline constexpr auto kLpakBlock = PakImage::Block(0);
    inline constexpr auto kSparseTable = SparseResources::Table(10);

    Sora::ErrorCode RegisterResources(const R::ResourceModule* desc, R::RegistrySink* sink) noexcept {
        if (sink == nullptr || sink->addLpak == nullptr || sink->addSparse == nullptr) {
            return Sora::ErrorCode::InvalidArgument;
        }
        if (const Sora::ErrorCode added = sink->addLpak(sink->registry, &kLpakBlock); added != Sora::ErrorCode::Ok) {
            return added;
        }
        return sink->addSparse(sink->registry, &kSparseTable);
    }

    inline constexpr R::ResourceModule kResourceModule{.name = "Sora.Resources.EmbeddedPakModule",
                                                       .moduleId =
                                                           R::HashUri("module://Sora.Resources.EmbeddedPakModule"),
                                                       .registerModule = RegisterResources};

} // namespace

REGISTER_RESOURCE_MODULE(kResourceModule)
