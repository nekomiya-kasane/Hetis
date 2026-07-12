#include <Sora/Core/Wire.h>
#include <Sora/Core/Resources/Resources.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace Sora::Resources;

namespace {

    [[nodiscard]] auto Bytes(std::string_view text) -> std::vector<std::byte> {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    [[nodiscard]] auto Text(std::span<const std::byte> bytes) -> std::string_view {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    [[nodiscard]] constexpr auto ResourceView(uint64_t hash, ResourceType type, std::string_view uri,
                                              const unsigned char* data, size_t size) -> ResourceBytesView {
        ResourceBytesView view{};
        view.hash = hash;
        view.type = type;
        view.uri = uri;
        view.data = data;
        view.size = size;
        return view;
    }

    [[nodiscard]] auto RewriteMetadataHash(std::vector<std::byte>& pak) -> Sora::VoidResult {
        size_t offset = 0;
        auto header = Sora::Wire::Read<FileHeader>(pak, offset);
        if (!header) {
            return std::unexpected(header.error());
        }

        header->metadataHash = 0;
        auto write = Sora::Wire::WriteAt(pak, 0, *header);
        if (!write) {
            return std::unexpected(write.error());
        }

        SectionDescriptor entries{};
        SectionDescriptor strings{};
        offset = static_cast<size_t>(header->sectionTableOffset);
        for (uint16_t i = 0; i < header->sectionCount; ++i) {
            auto section = Sora::Wire::Read<SectionDescriptor>(pak, offset);
            if (!section) {
                return std::unexpected(section.error());
            }
            if (static_cast<SectionKind>(section->kind) == SectionKind::Entries) {
                entries = *section;
            } else if (static_cast<SectionKind>(section->kind) == SectionKind::Strings) {
                strings = *section;
            }
        }

        auto hasher = Sora::Hashing::Hasher<>{};
        hasher.FeedBytes(std::span<const std::byte>{pak}.subspan(0, header->headerSize));
        hasher.FeedBytes(std::span<const std::byte>{pak}.subspan(static_cast<size_t>(header->sectionTableOffset),
                                                                 header->sectionSize * header->sectionCount));
        hasher.FeedBytes(std::span<const std::byte>{pak}.subspan(static_cast<size_t>(entries.offset),
                                                                 static_cast<size_t>(entries.size)));
        hasher.FeedBytes(std::span<const std::byte>{pak}.subspan(static_cast<size_t>(strings.offset),
                                                                 static_cast<size_t>(strings.size)));
        header->metadataHash = hasher.Finalize();
        return Sora::Wire::WriteAt(pak, 0, *header);
    }

    [[nodiscard]] auto RewriteFirstEntryflags(std::vector<std::byte>& pak, uint16_t flags) -> Sora::VoidResult {
        size_t offset = 0;
        auto header = Sora::Wire::Read<FileHeader>(pak, offset);
        if (!header) {
            return std::unexpected(header.error());
        }

        offset = static_cast<size_t>(header->sectionTableOffset);
        SectionDescriptor entries{};
        for (uint16_t i = 0; i < header->sectionCount; ++i) {
            auto section = Sora::Wire::Read<SectionDescriptor>(pak, offset);
            if (!section) {
                return std::unexpected(section.error());
            }
            if (static_cast<SectionKind>(section->kind) == SectionKind::Entries) {
                entries = *section;
            }
        }
        if (entries.size == 0) {
            return std::unexpected(Sora::ErrorCode::ResourceCorrupted);
        }

        offset = static_cast<size_t>(entries.offset);
        auto entry = Sora::Wire::Read<ResourceEntry>(pak, offset);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        entry->flags = flags;

        auto write = Sora::Wire::WriteAt(pak, static_cast<size_t>(entries.offset), *entry);
        if (!write) {
            return std::unexpected(write.error());
        }

        entries.checksum = Sora::Hashing::HashByteRange(std::span<const std::byte>{pak}.subspan(
            static_cast<size_t>(entries.offset), static_cast<size_t>(entries.size)));
        write = Sora::Wire::WriteAt(pak, static_cast<size_t>(header->sectionTableOffset), entries);
        if (!write) {
            return std::unexpected(write.error());
        }
        return RewriteMetadataHash(pak);
    }

} // namespace

namespace StaticResourceResolution {

    namespace [[= Sora::Resources::$::Resource{
        .uri = "res://image/ui", .type = Sora::Resources::ResourceType::Image, .extension = ".ktx2"}]] Ui {

        inline constexpr unsigned char Logo[] = {'l', 'o', 'g', 'o'};

        namespace Icons {

            inline constexpr unsigned char Close[] = {'x'};

        } // namespace Icons

        namespace [[= Sora::Resources::$::Resource{.uri = "dark"}]] Theme {

            inline constexpr unsigned char Close[] = {'d'};

        } // namespace Theme

        [[= Sora::Resources::$::Resource{.uri = "logo.webp"}]] inline constexpr unsigned char WebLogo[] = {'w'};

    } // namespace Ui

    namespace [[= Sora::Resources::$::Resource{
        .uri = "res://asset/root", .type = Sora::Resources::ResourceType::Raw, .extension = ".bin"}]] Assets {

        namespace [[= Sora::Resources::$::Resource{.uri = "data"}]] Tables {

            inline constexpr unsigned char Table[] = {'t'};

        } // namespace Tables

    } // namespace Assets

    namespace Shader {

        inline constexpr unsigned char Fullscreen[] = {'s'};

    } // namespace Shader

    namespace Image {

        [[= Sora::Resources::$::Resource{.extension = ".webp"}]] inline constexpr unsigned char Portrait[] = {'p'};

    } // namespace Image

} // namespace StaticResourceResolution

TEST_CASE("PakBuilder serializes and PakView reads lpak", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://core/config/app.json", ResourceType::Config, Bytes(R"({"name":"sora"})")).has_value());
    REQUIRE(builder.Add("res://core/shaders/fullscreen.slang", ResourceType::Shader, Bytes("shader-main")).has_value());

    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());

    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());
    REQUIRE(view->Count() == 2);
    REQUIRE(view->Header().major == kLpakMajor);

    auto data = view->Get(HashUri("res://core/shaders/fullscreen.slang"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "shader-main");

    auto* entry = view->Find(HashUri("res://core/config/app.json"));
    REQUIRE(entry != nullptr);
    auto uri = view->UriOf(*entry);
    REQUIRE(uri.has_value());
    REQUIRE(*uri == "res://core/config/app.json");
}

TEST_CASE("ResourceListOf resolves namespace prefixes and relative resource URIs", "[Sora][Resources]") {
    using ResourceList = ResourceListOf<^^StaticResourceResolution>;
    using SparseResources = StaticSparseTableFor<ResourceList>;
    using PakImage = StaticPakImage<ResourceList>;

    constexpr auto table = SparseResources::Table();
    REQUIRE(table.count == 7);

    auto findSparse = [&](std::string_view uri) -> const ModuleResourceEntry* {
        for (const ModuleResourceEntry& entry : std::span{table.entries, static_cast<size_t>(table.count)}) {
            if (std::string_view{entry.uri, entry.uriSize} == uri) {
                return &entry;
            }
        }
        return nullptr;
    };

    const auto* logo = findSparse("res://image/ui/logo.ktx2");
    REQUIRE(logo != nullptr);
    REQUIRE(static_cast<ResourceType>(logo->type) == ResourceType::Image);

    const auto* close = findSparse("res://image/ui/icons/close.ktx2");
    REQUIRE(close != nullptr);
    REQUIRE(static_cast<ResourceType>(close->type) == ResourceType::Image);

    const auto* dark = findSparse("res://image/ui/dark/close.ktx2");
    REQUIRE(dark != nullptr);
    REQUIRE(static_cast<ResourceType>(dark->type) == ResourceType::Image);

    const auto* webLogo = findSparse("res://image/ui/logo.webp");
    REQUIRE(webLogo != nullptr);
    REQUIRE(static_cast<ResourceType>(webLogo->type) == ResourceType::Image);

    const auto* tableEntry = findSparse("res://asset/root/data/table.bin");
    REQUIRE(tableEntry != nullptr);
    REQUIRE(static_cast<ResourceType>(tableEntry->type) == ResourceType::Raw);

    const auto* shader = findSparse("res://shader/fullscreen.wgsl");
    REQUIRE(shader != nullptr);
    REQUIRE(static_cast<ResourceType>(shader->type) == ResourceType::Shader);

    const auto* portrait = findSparse("res://image/portrait.webp");
    REQUIRE(portrait != nullptr);
    REQUIRE(static_cast<ResourceType>(portrait->type) == ResourceType::Image);

    auto view = PakView::Open(std::as_bytes(std::span{PakImage::kBytes.data(), PakImage::kBytes.size()}));
    REQUIRE(view.has_value());
    REQUIRE(view->Count() == table.count);
    REQUIRE(view->Find(HashUri("res://image/ui/icons/close.ktx2")) != nullptr);
    REQUIRE(view->Find(HashUri("res://image/ui/logo.webp")) != nullptr);
    REQUIRE(view->Find(HashUri("res://asset/root/data/table.bin")) != nullptr);
}

TEST_CASE("PakBuilder and StaticPakImage share the canonical lpak writer", "[Sora][Resources]") {
    using ResourceList = ResourceListOf<^^StaticResourceResolution>;
    using PakImage = StaticPakImage<ResourceList>;

    PakBuilder builder;
    REQUIRE(builder.Add("res://image/ui/dark/close.ktx2", ResourceType::Image, Bytes("d")).has_value());
    REQUIRE(builder.Add("res://asset/root/data/table.bin", ResourceType::Raw, Bytes("t")).has_value());
    REQUIRE(builder.Add("res://image/ui/logo.ktx2", ResourceType::Image, Bytes("logo")).has_value());
    REQUIRE(builder.Add("res://shader/fullscreen.wgsl", ResourceType::Shader, Bytes("s")).has_value());
    REQUIRE(builder.Add("res://image/ui/logo.webp", ResourceType::Image, Bytes("w")).has_value());
    REQUIRE(builder.Add("res://image/portrait.webp", ResourceType::Image, Bytes("p")).has_value());
    REQUIRE(builder.Add("res://image/ui/icons/close.ktx2", ResourceType::Image, Bytes("x")).has_value());

    auto runtimePak = builder.Serialize();
    REQUIRE(runtimePak.has_value());

    auto staticPak = std::as_bytes(std::span{PakImage::kBytes.data(), PakImage::kBytes.size()});
    REQUIRE(runtimePak->size() == staticPak.size());
    REQUIRE(std::ranges::equal(*runtimePak, staticPak));
}

TEST_CASE("PakBuilder accepts borrowed resource byte views", "[Sora][Resources]") {
    static constexpr unsigned char kText[] = {'h', 'e', 'l', 'l', 'o'};

    PakBuilder builder;
    REQUIRE(builder
                .Add(ResourceView(HashUri("res://borrowed/text.txt"), ResourceType::Raw, "res://borrowed/text.txt",
                                  kText, std::size(kText)))
                .has_value());
    REQUIRE(builder
                .Add(ResourceView(HashUri("res://borrowed/empty.bin"), ResourceType::Raw, "res://borrowed/empty.bin",
                                  nullptr, 0))
                .has_value());
    REQUIRE_FALSE(builder
                      .Add(ResourceView(HashUri("res://borrowed/bad.bin"), ResourceType::Raw, "res://borrowed/bad.bin",
                                        nullptr, 1))
                      .has_value());

    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());
    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());

    auto data = view->Get(HashUri("res://borrowed/text.txt"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "hello");

    data = view->Get(HashUri("res://borrowed/empty.bin"));
    REQUIRE(data.has_value());
    REQUIRE(data->empty());
}

TEST_CASE("ResourceRegistry resolves higher-priority directly mounted packages", "[Sora][Resources]") {
    PakBuilder low;
    PakBuilder high;
    REQUIRE(low.Add("res://shared/value.txt", ResourceType::Raw, Bytes("low")).has_value());
    REQUIRE(high.Add("res://shared/value.txt", ResourceType::Raw, Bytes("high")).has_value());

    auto lowPak = low.Serialize();
    auto highPak = high.Serialize();
    REQUIRE(lowPak.has_value());
    REQUIRE(highPak.has_value());

    ResourceRegistry registry;
    REQUIRE(registry.MountPak("low", std::move(*lowPak), 0).has_value());
    REQUIRE(registry.MountPak("high", std::move(*highPak), 10).has_value());
    REQUIRE(registry.ModuleCount() == 0);
    REQUIRE(registry.LayoutCount() == 2);
    REQUIRE(registry.ResourceCount() == 1);

    auto data = registry.Open("res://shared/value.txt");
    REQUIRE(data.has_value());
    REQUIRE(Text(data->Bytes()) == "high");
}

TEST_CASE("PakView rejects corrupted package bytes", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://x/raw.bin", ResourceType::Raw, Bytes("payload")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());
    REQUIRE(pak->size() > 8);
    (*pak)[8] = std::byte{0x7F};

    auto view = PakView::Open(*pak);
    REQUIRE_FALSE(view.has_value());
    REQUIRE(view.error() == Sora::ErrorCode::ResourceCorrupted);
}

TEST_CASE("PakView mount validation does not scan payload bytes", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://lazy/raw.bin", ResourceType::Raw, Bytes("payload")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());

    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());
    const auto& entry = view->Entries()[0];
    (*pak)[static_cast<size_t>(entry.payloadOffset)] = std::byte{'P'};

    auto reopened = PakView::Open(*pak);
    REQUIRE(reopened.has_value());
    auto data = reopened->Get(HashUri("res://lazy/raw.bin"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "Payload");
}

TEST_CASE("PakBuilder accepts empty payloads and rejects duplicate semantic ids", "[Sora][Resources]") {
    PakBuilder empty;
    REQUIRE(empty.Add("res://empty/blob.bin", ResourceType::Raw, {}).has_value());
    auto pak = empty.Serialize();
    REQUIRE(pak.has_value());

    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());
    auto data = view->Get(HashUri("res://empty/blob.bin"));
    REQUIRE(data.has_value());
    REQUIRE(data->empty());

    PakBuilder duplicate;
    REQUIRE(duplicate.Add("res://dup/value.txt", ResourceType::Raw, Bytes("a")).has_value());
    REQUIRE(duplicate.Add("res://dup/value.txt", ResourceType::Raw, Bytes("b")).has_value());
    auto duplicatePak = duplicate.Serialize();
    REQUIRE_FALSE(duplicatePak.has_value());
    REQUIRE(duplicatePak.error() == Sora::ErrorCode::InvalidArgument);
}

TEST_CASE("ResourceRegistry mounts borrowed packages", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://io/value.txt", ResourceType::Raw, Bytes("written")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());

    ResourceRegistry registry;
    REQUIRE(registry.MountEmbeddedPak("borrowed", std::span<const std::byte>{pak->data(), pak->size()}).has_value());
    auto borrowed = registry.Open("res://io/value.txt");
    REQUIRE(borrowed.has_value());
    REQUIRE(Text(borrowed->Bytes()) == "written");
}

TEST_CASE("PakBuilder normalizes URIs and rejects malformed resource ids", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://folder\\shader.slang", ResourceType::Shader, Bytes("shader")).has_value());

    std::string invalid = "res://bad";
    invalid.push_back('\0');
    invalid += "name";
    REQUIRE_FALSE(builder.Add(invalid, ResourceType::Raw, Bytes("x")).has_value());
    REQUIRE_FALSE(builder.Add("bad/type", ResourceType::Raw, Bytes("x")).has_value());
    REQUIRE_FALSE(builder.Add("res://bad/type", ResourceType::Unknown, Bytes("x")).has_value());
    REQUIRE_FALSE(builder.Add("res://bad/type", static_cast<ResourceType>(0xFFFF), Bytes("x")).has_value());

    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());
    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());

    auto data = view->Get(HashUri("res://folder/shader.slang"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "shader");

    auto* entry = view->Find(HashUri("res://folder/shader.slang"));
    REQUIRE(entry != nullptr);
    auto uri = view->UriOf(*entry);
    REQUIRE(uri.has_value());
    REQUIRE(*uri == "res://folder/shader.slang");
}

TEST_CASE("ResourceRegistry mount names and equal-priority overlays are deterministic", "[Sora][Resources]") {
    PakBuilder first;
    PakBuilder second;
    REQUIRE(first.Add("res://shared/equal.txt", ResourceType::Raw, Bytes("first")).has_value());
    REQUIRE(second.Add("res://shared/equal.txt", ResourceType::Raw, Bytes("second")).has_value());

    auto firstPak = first.Serialize();
    auto secondPak = second.Serialize();
    REQUIRE(firstPak.has_value());
    REQUIRE(secondPak.has_value());

    ResourceRegistry registry;
    REQUIRE_FALSE(registry.MountPak("", std::vector<std::byte>{}, 0).has_value());
    REQUIRE(registry.MountPak("overlay", std::move(*firstPak), 0).has_value());
    REQUIRE_FALSE(registry.MountPak("overlay", std::move(*secondPak), 0).has_value());

    auto secondPakAgain = second.Serialize();
    REQUIRE(secondPakAgain.has_value());
    REQUIRE(registry.MountPak("overlay-2", std::move(*secondPakAgain), 0).has_value());

    auto data = registry.Open("res://shared/equal.txt");
    REQUIRE(data.has_value());
    REQUIRE(Text(data->Bytes()) == "second");
}

TEST_CASE("PakView rejects forged entry descriptors", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://secure/value.txt", ResourceType::Raw, Bytes("payload")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());

    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());
    REQUIRE(view->Count() == 1);

    auto forged = view->Entries()[0];
    forged.packedSize = 0;
    auto data = view->DataOf(forged);
    REQUIRE_FALSE(data.has_value());
    REQUIRE(data.error() == Sora::ErrorCode::ResourceCorrupted);
}

TEST_CASE("PakView rejects non-canonical entry flags", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://future/streamed.bin", ResourceType::Raw, Bytes("payload")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());
    REQUIRE(RewriteFirstEntryflags(*pak, 1u).has_value());

    auto view = PakView::Open(*pak);
    REQUIRE_FALSE(view.has_value());
    REQUIRE(view.error() == Sora::ErrorCode::ResourceCorrupted);
}
