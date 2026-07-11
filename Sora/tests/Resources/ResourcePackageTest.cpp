#include <Sora/Resources/Resources.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace Sora::Resources;
using namespace Sora::Literals;

namespace {

    [[nodiscard]] auto Bytes(std::string_view text) -> std::vector<std::byte> {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    [[nodiscard]] auto Text(std::span<const std::byte> bytes) -> std::string_view {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    [[nodiscard]] auto RewriteHeader(std::vector<std::byte>& pak, FileHeader header) -> Sora::VoidResult {
        header.headerHash = 0;
        header.fileHash = 0;
        std::vector<std::byte> headerBytes;
        Wire::Append(headerBytes, header);
        header.headerHash = HashBytes(headerBytes);
        auto write = Wire::WriteAt(pak, 0, header);
        if (!write) {
            return std::unexpected(write.error());
        }
        header.fileHash = HashBytes(pak);
        return Wire::WriteAt(pak, 0, header);
    }

    [[nodiscard]] auto RewriteFirstEntryflags(std::vector<std::byte>& pak, uint16_t flags) -> Sora::VoidResult {
        size_t offset = 0;
        auto header = Wire::Read<FileHeader>(pak, offset);
        if (!header) {
            return std::unexpected(header.error());
        }

        offset = static_cast<size_t>(header->sectionTableOffset);
        SectionDescriptor entries{};
        for (uint16_t i = 0; i < header->sectionCount; ++i) {
            auto section = Wire::Read<SectionDescriptor>(pak, offset);
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
        auto entry = Wire::Read<ResourceEntry>(pak, offset);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        entry->flags = flags;

        auto write = Wire::WriteAt(pak, static_cast<size_t>(entries.offset), *entry);
        if (!write) {
            return std::unexpected(write.error());
        }

        entries.checksum = HashBytes(std::span<const std::byte>{pak}.subspan(static_cast<size_t>(entries.offset),
                                                                             static_cast<size_t>(entries.size)));
        write = Wire::WriteAt(pak, static_cast<size_t>(header->sectionTableOffset), entries);
        if (!write) {
            return std::unexpected(write.error());
        }
        return RewriteHeader(pak, *header);
    }

} // namespace

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

TEST_CASE("StaticBundle provides compile-time embedded resources", "[Sora][Resources]") {
    static constexpr unsigned char kText[] = {'h', 'e', 'l', 'l', 'o'};
    static constexpr unsigned char kJson[] = {'{', '}', '\n'};

    constexpr auto text = MakeEmbeddedResource<"res://bundle/text.txt"_FS, ResourceType::Raw>(kText);
    constexpr auto json = MakeEmbeddedResource<"res://bundle/config.json"_FS, ResourceType::Config>(kJson);
    constexpr auto bundle = MakeStaticBundle(text, json);

    static_assert(bundle.Count() == 2);

    auto data = bundle.Get(HashUri("res://bundle/text.txt"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "hello");

    auto pak = bundle.ToPakBytes();
    REQUIRE(pak.has_value());
    auto view = PakView::Open(*pak);
    REQUIRE(view.has_value());
    REQUIRE(view->Count() == 2);
}

TEST_CASE("StaticBundle validates views and permits empty payloads", "[Sora][Resources]") {
    std::array<EmbeddedResourceView, 1> empty{EmbeddedResourceView{
        .Hash = HashUri("res://bundle/empty.bin"),
        .Type = ResourceType::Raw,
        .Data = nullptr,
        .Size = 0,
        .Uri = "res://bundle/empty.bin",
    }};
    StaticBundle<1> bundle(empty);
    auto data = bundle.Get(HashUri("res://bundle/empty.bin"));
    REQUIRE(data.has_value());
    REQUIRE(data->empty());

    std::array<EmbeddedResourceView, 1> forged{EmbeddedResourceView{
        .Hash = HashUri("res://bundle/other.bin"),
        .Type = ResourceType::Raw,
        .Data = nullptr,
        .Size = 0,
        .Uri = "res://bundle/empty.bin",
    }};
    REQUIRE_THROWS_AS(StaticBundle<1>(forged), const char*);

    std::array<EmbeddedResourceView, 1> nonCanonical{EmbeddedResourceView{
        .Hash = HashUri("res://bundle\\empty.bin"),
        .Type = ResourceType::Raw,
        .Data = nullptr,
        .Size = 0,
        .Uri = "res://bundle\\empty.bin",
    }};
    REQUIRE_THROWS_AS(StaticBundle<1>(nonCanonical), const char*);
}

TEST_CASE("ResourceStore resolves higher-priority mounted packages", "[Sora][Resources]") {
    PakBuilder low;
    PakBuilder high;
    REQUIRE(low.Add("res://shared/value.txt", ResourceType::Raw, Bytes("low")).has_value());
    REQUIRE(high.Add("res://shared/value.txt", ResourceType::Raw, Bytes("high")).has_value());

    auto lowPak = low.Serialize();
    auto highPak = high.Serialize();
    REQUIRE(lowPak.has_value());
    REQUIRE(highPak.has_value());

    ResourceStore store;
    REQUIRE(store.MountPak("low", std::move(*lowPak), 0).has_value());
    REQUIRE(store.MountPak("high", std::move(*highPak), 10).has_value());
    REQUIRE(store.MountCount() == 2);
    REQUIRE(store.ResourceCount() == 1);

    auto data = store.Get(HashUri("res://shared/value.txt"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "high");
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

TEST_CASE("ResourceStore mounts borrowed packages", "[Sora][Resources]") {
    PakBuilder builder;
    REQUIRE(builder.Add("res://io/value.txt", ResourceType::Raw, Bytes("written")).has_value());
    auto pak = builder.Serialize();
    REQUIRE(pak.has_value());

    ResourceStore store;
    REQUIRE(store.MountEmbeddedPak("borrowed", std::span<const std::byte>{pak->data(), pak->size()}).has_value());
    auto borrowed = store.Get(HashUri("res://io/value.txt"));
    REQUIRE(borrowed.has_value());
    REQUIRE(Text(*borrowed) == "written");
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

TEST_CASE("ResourceStore mount names and equal-priority overlays are deterministic", "[Sora][Resources]") {
    PakBuilder first;
    PakBuilder second;
    REQUIRE(first.Add("res://shared/equal.txt", ResourceType::Raw, Bytes("first")).has_value());
    REQUIRE(second.Add("res://shared/equal.txt", ResourceType::Raw, Bytes("second")).has_value());

    auto firstPak = first.Serialize();
    auto secondPak = second.Serialize();
    REQUIRE(firstPak.has_value());
    REQUIRE(secondPak.has_value());

    ResourceStore store;
    REQUIRE_FALSE(store.MountPak("", std::vector<std::byte>{}, 0).has_value());
    REQUIRE(store.MountPak("overlay", std::move(*firstPak), 0).has_value());
    REQUIRE_FALSE(store.MountPak("overlay", std::move(*secondPak), 0).has_value());

    auto secondPakAgain = second.Serialize();
    REQUIRE(secondPakAgain.has_value());
    REQUIRE(store.MountPak("overlay-2", std::move(*secondPakAgain), 0).has_value());

    auto data = store.Get(HashUri("res://shared/equal.txt"));
    REQUIRE(data.has_value());
    REQUIRE(Text(*data) == "second");
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
