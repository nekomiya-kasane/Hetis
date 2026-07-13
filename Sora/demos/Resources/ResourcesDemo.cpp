#include "Sora/Core/Resources/Resources.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace R = Sora::Resources;
using namespace Sora::Literals;

namespace DemoStaticResources {

    namespace Shader {

        inline constexpr unsigned char Fullscreen[] =
            "@vertex fn main() -> @builtin(position) vec4f { return vec4f(); }";

    } // namespace Shader

    namespace Material {

        inline constexpr unsigned char Demo[] = R"({"baseColor":[1.0,0.5,0.25,1.0],"roughness":0.72})";

    } // namespace Material

    namespace Config {

        inline constexpr unsigned char Window[] = "window.width=1280\nwindow.height=720\n";

    } // namespace Config

} // namespace DemoStaticResources

namespace {

    constexpr unsigned char kShaderBytes[] = "@vertex fn main() -> @builtin(position) vec4f { return vec4f(); }";
    constexpr unsigned char kMaterialBytes[] = R"({"baseColor":[1.0,0.5,0.25,1.0],"roughness":0.72})";
    constexpr unsigned char kConfigBytes[] = "window.width=1280\nwindow.height=720\n";
    constexpr unsigned char kOverrideConfigBytes[] = "window.width=1920\nwindow.height=1080\n";

    [[nodiscard]] auto TextBytes(std::string_view text) -> std::vector<std::byte> {
        return text |
               std::views::transform([](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); }) |
               std::ranges::to<std::vector>();
    }

    [[nodiscard]] auto RawBytes(const unsigned char* data, size_t size) -> std::span<const std::byte> {
        return std::as_bytes(std::span{data, size});
    }

    template<typename T>
    void Require(const Sora::Result<T>& result) {
        assert(result.has_value());
    }

    void Require(const Sora::VoidResult& result) {
        assert(result.has_value());
    }

    void ResourceIdDemo() {
        static_assert(Sora::IsUri("sora://host/path/to/item?a=b&flag&empty=&encoded=a%20b&plus=a+b#frag"));
        static constexpr auto requestUri = "sora://host/path/to/item?a=b&flag&empty=&encoded=a%20b&plus=a+b#frag"_URI;
        static_assert(requestUri.Scheme() == "sora");
        static_assert(requestUri.Authority() == "host");
        static_assert(requestUri.Path() == "/path/to/item");
        static_assert(requestUri.QueryParams().Contains("flag"));
        static_assert(requestUri.QueryParams().Get("a") == "b");
        static_assert(requestUri.HasAnchor());
        static_assert(requestUri.Anchor() == "frag");
        static_assert(requestUri.Fragment() == requestUri.Anchor());
        static_assert(requestUri.QueryParams().Get("empty").empty());
        constexpr auto flagParam = requestUri.QueryParams().Find("flag");
        static_assert(flagParam.has_value() && !flagParam->hasEquals);
        constexpr auto parsedRequest = requestUri.Parsed();
        static_assert(parsedRequest.has_value());
        static_assert(parsedRequest->Scheme() == "sora");
        static_assert(parsedRequest->Authority() == "host");
        static_assert(parsedRequest->Path() == "/path/to/item");
        static_assert(parsedRequest->QueryParams().Contains("flag"));
        static_assert(parsedRequest->Hash() == requestUri.Hash());
        constexpr auto madeUri = Sora::MakeUri<128>("sora://host/root");
        static_assert(madeUri.has_value());
        static_assert(*madeUri == "sora://host/root");
        constexpr auto composedUri = Sora::ComposeUri<128>(Sora::UriParts{
            .scheme = "sora",
            .authority = "host",
            .path = "/root",
            .query = "mode=debug",
            .fragment = "main",
            .hasAuthority = true,
            .hasQuery = true,
            .hasFragment = true,
        });
        static_assert(composedUri.has_value());
        static_assert(composedUri->view() == "sora://host/root?mode=debug#main");
        constexpr auto literalPathUri = "sora://host/root"_URI.AppendPath<16>("shader");
        static_assert(literalPathUri.has_value());
        static_assert(literalPathUri->view() == "sora://host/root/shader");
        constexpr auto pathBuilt = "sora://host/root?mode=debug#main"_URI.AppendPath<16>("shader");
        static_assert(pathBuilt.has_value());
        constexpr auto queryBuilt = pathBuilt->AppendQuery<32>(Sora::UriQueryArgument::Pair("variant", "asan"));
        static_assert(queryBuilt.has_value());
        static_assert(queryBuilt->view() == "sora://host/root/shader?mode=debug&variant=asan#main");
        constexpr auto anchoredUri = queryBuilt->WithAnchor("entry");
        static_assert(anchoredUri.has_value());
        static_assert(anchoredUri->view() == "sora://host/root/shader?mode=debug&variant=asan#entry");
        constexpr auto builtUri = Sora::UriBuilder<128>{}
                                      .Scheme("sora")
                                      .Authority("host")
                                      .Path("root")
                                      .Segment("shader")
                                      .Query("variant", "asan")
                                      .Flag("cache")
                                      .Anchor("entry")
                                      .Build();
        static_assert(builtUri.has_value());
        static_assert(builtUri->view() == "sora://host/root/shader?variant=asan&cache#entry");
        constexpr auto rewrittenUri = Sora::UriBuilder<128>::From(*builtUri).NoQuery().Query("mode", "debug").Build();
        static_assert(rewrittenUri.has_value());
        static_assert(rewrittenUri->view() == "sora://host/root/shader?mode=debug#entry");
        constexpr auto decoded = Sora::PercentDecode<16>("a%20b");
        static_assert(decoded.has_value());
        static_assert(decoded->view() == "a b");
        constexpr auto formDecoded = Sora::PercentDecodeFormComponent<16>("a+b");
        static_assert(formDecoded.has_value());
        static_assert(formDecoded->view() == "a b");
        constexpr auto normalized = Sora::NormalizeUriSyntax<64>("SORA://Host/%2f?a=%3a");
        static_assert(normalized.has_value());
        static_assert(normalized->view() == "sora://Host/%2F?a=%3A");

        static_assert(R::IsCanonicalUri("res://shader/fullscreen.wgsl"));
        static_assert(!R::IsCanonicalUri("res://shader\\fullscreen.wgsl"));
        static_assert(!R::IsCanonicalUri("res://shader/fullscreen.wgsl?variant=debug"));
        static_assert(!R::IsCanonicalUri("res://shader/fullscreen.wgsl#entry"));
        static_assert(R::HashUri("res://shader/fullscreen.wgsl") != R::HashUri("res://shader\\fullscreen.wgsl"));
        constexpr auto resourceUri = R::ParseResourceUri("res://shader/fullscreen.wgsl");
        static_assert(resourceUri.has_value());
        static_assert(resourceUri->Scheme() == "res");
        static_assert(resourceUri->Authority() == "shader");
        static_assert(resourceUri->Path() == "/fullscreen.wgsl");
        static_assert(resourceUri->Hash() == R::HashUri("res://shader/fullscreen.wgsl"));

        constexpr auto shaderId = R::StaticResourceId<"res://shader/fullscreen.wgsl"_FS, R::ResourceType::Shader>{};
        static_assert(shaderId.kType == R::ResourceType::Shader);
        static_assert(shaderId.kHash == R::HashUri("res://shader/fullscreen.wgsl"));

        R::ResourceBytesView shader{};
        shader.hash = shaderId.kHash;
        shader.type = R::ResourceType::Shader;
        shader.uri = "res://shader/fullscreen.wgsl";
        shader.data = kShaderBytes;
        shader.size = sizeof(kShaderBytes);
        assert(shader.hash == shaderId.kHash);
        assert(shader.type == R::ResourceType::Shader);
        assert(shader.uri == "res://shader/fullscreen.wgsl");
        assert(std::ranges::equal(shader.Bytes(), RawBytes(kShaderBytes, sizeof(kShaderBytes))));
    }

    void StaticResourceModuleDemo() {
        using ResourceList = R::ResourceListOf<^^DemoStaticResources>;
        using SparseResources = R::StaticSparseTableFor<ResourceList>;
        using PakImage = R::StaticPakImage<ResourceList>;

        constexpr auto table = SparseResources::Table();
        assert(table.count == 3);

        auto pak = R::PakView::Open(std::as_bytes(std::span{PakImage::kBytes.data(), PakImage::kBytes.size()}));
        Require(pak);
        assert(pak->Count() == 3);

        auto materialBytes = pak->Get(R::HashUri("res://material/demo.json"));
        Require(materialBytes);
        assert(std::ranges::equal(*materialBytes, RawBytes(kMaterialBytes, sizeof(kMaterialBytes))));

        auto configBytes = pak->Get(R::HashUri("res://config/window.ini"));
        Require(configBytes);
        assert(std::ranges::equal(*configBytes, RawBytes(kConfigBytes, sizeof(kConfigBytes))));

        auto missing = pak->Get(R::HashUri("res://missing.asset"));
        assert(!missing && missing.error() == Sora::ErrorCode::ResourceNotFound);
    }

    void PakBuilderAndViewDemo() {
        R::PakBuilder builder;
        Require(builder.Add("res://shader/fullscreen.wgsl", R::ResourceType::Shader,
                            RawBytes(kShaderBytes, sizeof(kShaderBytes))));
        Require(builder.Add("res://material/demo.json", R::ResourceType::Material,
                            RawBytes(kMaterialBytes, sizeof(kMaterialBytes))));
        Require(builder.Add("res://config/window.ini", R::ResourceType::Config,
                            RawBytes(kConfigBytes, sizeof(kConfigBytes))));
        assert(builder.Count() == 3);

        auto invalidUri = builder.Add("shader/fullscreen.wgsl", R::ResourceType::Shader,
                                      RawBytes(kShaderBytes, sizeof(kShaderBytes)));
        assert(!invalidUri && invalidUri.error() == Sora::ErrorCode::InvalidArgument);

        auto invalidType = builder.Add("res://invalid/type", R::ResourceType::Unknown, {});
        assert(!invalidType && invalidType.error() == Sora::ErrorCode::InvalidArgument);

        auto pakBytes = builder.Serialize();
        Require(pakBytes);

        auto pak = R::PakView::Open(*pakBytes);
        Require(pak);
        assert(pak->Count() == 3);
        assert(pak->Header().magic == R::kLpakMagic);
        assert(pak->Header().resourceCount == 3);

        auto* configEntry = pak->Find(R::HashUri("res://config/window.ini"));
        assert(configEntry != nullptr);
        assert(configEntry->type == static_cast<uint16_t>(R::ResourceType::Config));

        auto uri = pak->UriOf(*configEntry);
        Require(uri);
        assert(*uri == "res://config/window.ini");

        auto configBytes = pak->DataOf(*configEntry);
        Require(configBytes);
        assert(std::ranges::equal(*configBytes, RawBytes(kConfigBytes, sizeof(kConfigBytes))));

        auto byHash = pak->Get(R::HashUri("res://shader/fullscreen.wgsl"));
        Require(byHash);
        assert(std::ranges::equal(*byHash, RawBytes(kShaderBytes, sizeof(kShaderBytes))));

        auto missing = pak->Get(R::HashUri("res://missing.asset"));
        assert(!missing && missing.error() == Sora::ErrorCode::ResourceNotFound);

        auto corruptedBytes = *pakBytes;
        corruptedBytes[8] = static_cast<std::byte>(static_cast<uint8_t>(corruptedBytes[8]) ^ 0xFFu);
        auto corrupted = R::PakView::Open(corruptedBytes);
        assert(!corrupted && corrupted.error() == Sora::ErrorCode::ResourceCorrupted);

        R::PakBuilder duplicateBuilder;
        Require(duplicateBuilder.Add("res://config/window.ini", R::ResourceType::Config,
                                     RawBytes(kConfigBytes, sizeof(kConfigBytes))));
        Require(duplicateBuilder.Add("res://config/window.ini", R::ResourceType::Config,
                                     RawBytes(kOverrideConfigBytes, sizeof(kOverrideConfigBytes))));
        auto duplicated = duplicateBuilder.Serialize();
        assert(!duplicated && duplicated.error() == Sora::ErrorCode::InvalidArgument);
    }

    void ResourceRegistryMountDemo() {
        R::PakBuilder baseBuilder;
        Require(baseBuilder.Add("res://config/window.ini", R::ResourceType::Config,
                                RawBytes(kConfigBytes, sizeof(kConfigBytes))));
        Require(baseBuilder.Add("res://shader/fullscreen.wgsl", R::ResourceType::Shader,
                                RawBytes(kShaderBytes, sizeof(kShaderBytes))));
        auto basePak = baseBuilder.Serialize();
        Require(basePak);

        R::PakBuilder overrideBuilder;
        Require(overrideBuilder.Add("res://config/window.ini", R::ResourceType::Config,
                                    RawBytes(kOverrideConfigBytes, sizeof(kOverrideConfigBytes))));
        auto overridePak = overrideBuilder.Serialize();
        Require(overridePak);

        R::ResourceRegistry registry;
        Require(registry.MountPak("base", std::move(*basePak), 0));
        assert(registry.ModuleCount() == 0);
        assert(registry.LayoutCount() == 1);
        assert(registry.ResourceCount() == 2);

        auto baseConfig = registry.Open("res://config/window.ini");
        Require(baseConfig);
        assert(std::ranges::equal(baseConfig->Bytes(), RawBytes(kConfigBytes, sizeof(kConfigBytes))));

        Require(registry.MountPak("override", std::move(*overridePak), 10));
        assert(registry.LayoutCount() == 2);
        assert(registry.ResourceCount() == 2);

        auto overrideConfig = registry.Open("res://config/window.ini");
        Require(overrideConfig);
        assert(std::ranges::equal(overrideConfig->Bytes(),
                                  RawBytes(kOverrideConfigBytes, sizeof(kOverrideConfigBytes))));

        auto duplicateMount = registry.MountPak("base", TextBytes("not a pak"), 100);
        assert(!duplicateMount && duplicateMount.error() == Sora::ErrorCode::InvalidArgument);

        auto missing = registry.Open("res://not-mounted.asset");
        assert(!missing && missing.error() == Sora::ErrorCode::ResourceNotFound);
    }

} // namespace

int main() {
    ResourceIdDemo();
    StaticResourceModuleDemo();
    PakBuilderAndViewDemo();
    ResourceRegistryMountDemo();

    std::println("Resources demo passed: reflected static resources, lpak builder/view, registry priority, and error "
                 "paths verified.");
    return 0;
}
