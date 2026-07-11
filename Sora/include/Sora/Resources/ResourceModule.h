/**
 * @file ResourceModule.h
 * @brief Dynamic resource-module disclosure protocol and producer-side resource table helpers.
 * @ingroup Resources
 */
#pragma once

#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/StringUtils.h>
#include <Sora/Resources/EmbeddedResource.h>
#include <Sora/Resources/Format.h>
#include <Sora/Resources/ResourceId.h>
#include <Sora/Resources/Wire.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Sora::Resources {

    /** @brief ABI Magic for resource module descriptors, encoded as little-endian bytes `SRMD`. */
    inline constexpr uint32_t kResourceModuleMagic =
        uint32_t{'S'} | (uint32_t{'R'} << 8) | (uint32_t{'M'} << 16) | (uint32_t{'D'} << 24);

    /** @brief ABI Major version accepted by the current resource registry. */
    inline constexpr uint16_t kResourceModuleAbiMajor = 1;

    /** @brief ABI Minor version accepted by the current resource registry. */
    inline constexpr uint16_t kResourceModuleAbiMinor = 0;

    /** @brief Exported symbol name used by dynamic resource modules. */
    inline constexpr std::string_view kResourceModuleSymbol = "ResourceModule";

    /** @brief Header embedded in every cross-module resource protocol object. */
    struct AbiHeader {
        uint32_t magic = kResourceModuleMagic;    /**< Protocol family magic. */
        uint16_t major = kResourceModuleAbiMajor; /**< Breaking ABI version. */
        uint16_t minor = kResourceModuleAbiMinor; /**< Non-breaking ABI extension version. */
        uint32_t structSize = 0;                  /**< Size of the concrete structure carrying this header. */
        uint32_t reserved = 0;                    /**< Reserved for alignment and future flags. */
    };

    /** @brief Physical representation family disclosed by a resource module. */
    enum class ModuleLayout : uint8_t {
        Lpak = 1,     /**< One canonical `.lpak` byte image. */
        Sparse = 2,   /**< A sorted or unsorted table of independent static payloads. */
        Provider = 3, /**< A callback provider with indexed resources. */
    };

    /** @brief Immutable byte payload returned by provider callbacks. */
    struct ResourcePayload {
        const unsigned char* data = nullptr; /**< First payload byte, or @c nullptr for an empty payload. */
        uint64_t size = 0;                   /**< Payload size in bytes. */
    };

    /** @brief One resource descriptor in sparse tables and provider indexes. */
    struct ModuleResourceEntry {
        uint64_t semanticHash = 0; /**< Canonical URI hash. */
        uint64_t contentHash = 0;  /**< Payload hash. */
        /** @brief Canonical URI string, not necessarily NUL-terminated. */
        const char* uri = nullptr;
        uint32_t uriSize = 0;                                           /**< URI byte length. */
        uint16_t type = static_cast<uint16_t>(ResourceType::Unknown);   /**< Resource semantic type. */
        uint16_t codec = static_cast<uint16_t>(CompressionCodec::None); /**< Payload codec. */
        uint16_t flags = static_cast<uint16_t>(ResourceFlags::None);    /**< Storage flags. */
        uint16_t reserved = 0;                                          /**< Reserved for ABI alignment. */
        /** @brief Payload bytes for sparse tables; optional for providers. */
        const unsigned char* data = nullptr;
        /** @brief Payload size for sparse tables; optional for providers. */
        uint64_t size = 0;
    };

    /** @brief Disclosed canonical `.lpak` byte image. */
    struct LpakBlock {
        AbiHeader header{.structSize = sizeof(LpakBlock)}; /**< Protocol object header. */
        const unsigned char* bytes = nullptr;              /**< First byte of the `.lpak` image. */
        uint64_t size = 0;                                 /**< Exact `.lpak` byte size. */
        int32_t priority = 0;                              /**< Higher priority overrides equal semantic hashes. */
        uint32_t reserved = 0;                             /**< Reserved for ABI alignment. */
    };

    /** @brief Disclosed sparse table of static resources. */
    struct SparseTable {
        AbiHeader header{.structSize = sizeof(SparseTable)}; /**< Protocol object header. */
        const ModuleResourceEntry* entries = nullptr;        /**< Resource descriptors. */
        uint64_t count = 0;                                  /**< Number of descriptors. */
        int32_t priority = 0;                                /**< Higher priority overrides equal semantic hashes. */
        uint32_t reserved = 0;                               /**< Reserved for ABI alignment. */
    };

    /** @brief Callback provider for custom resource layouts. */
    struct ResourceProvider {
        using OpenFn = ErrorCode(void* context, uint64_t hash, ResourcePayload* out) noexcept;

        AbiHeader header{.structSize = sizeof(ResourceProvider)}; /**< Protocol object header. */
        void* context = nullptr;                                  /**< Provider-owned opaque context. */
        const ModuleResourceEntry* entries = nullptr;             /**< Index descriptors exposed by the provider. */
        uint64_t count = 0;                                       /**< Number of descriptors. */
        int32_t priority = 0;  /**< Higher priority overrides equal semantic hashes. */
        uint32_t reserved = 0; /**< Reserved for ABI alignment. */
        OpenFn* open = nullptr;
    };

    /** @brief Registry sink passed to a dynamic module during explicit resource registration. */
    struct RegistrySink {
        using AddLpakFn = ErrorCode(void* registry, const LpakBlock* block) noexcept;
        using AddSparseFn = ErrorCode(void* registry, const SparseTable* table) noexcept;
        using AddProviderFn = ErrorCode(void* registry, const ResourceProvider* provider) noexcept;

        AbiHeader header{.structSize = sizeof(RegistrySink)}; /**< Protocol object header. */
        void* registry = nullptr;                             /**< Registry-owned opaque state. */
        AddLpakFn* addLpak = nullptr;
        AddSparseFn* addSparse = nullptr;
        AddProviderFn* addProvider = nullptr;
    };

    /** @brief Dynamic resource module descriptor returned by the exported @c ResourceModule function. */
    struct ResourceModule {
        using RegisterModuleFn = ErrorCode(const ResourceModule* module, RegistrySink* sink) noexcept;

        AbiHeader header{.structSize = sizeof(ResourceModule)}; /**< Protocol object header. */
        const char* name = nullptr;                             /**< Human-readable stable module name. */
        uint64_t moduleId = 0;                                  /**< Stable module identity hash. */
        uint32_t flags = 0;                                     /**< Reserved module flags. */
        uint32_t reserved = 0;                                  /**< Reserved for ABI alignment. */
        RegisterModuleFn* registerModule = nullptr;
    };

    /** @brief Exported function type implemented by resource modules. */
    using ResourceModuleExportFn = const ResourceModule*() noexcept;

    /** @brief Return true when @p header describes a protocol object compatible with the current registry. */
    [[nodiscard]] constexpr bool IsCompatible(const AbiHeader& header, size_t minSize) noexcept {
        return header.magic == kResourceModuleMagic && header.major == kResourceModuleAbiMajor &&
               header.structSize >= minSize;
    }

    /** @brief Return payload bytes for a sparse-table entry. */
    [[nodiscard]] inline auto BytesOf(const ModuleResourceEntry& entry) noexcept -> std::span<const std::byte> {
        if (entry.size == 0) {
            return {};
        }
        return {reinterpret_cast<const std::byte*>(entry.data), static_cast<size_t>(entry.size)};
    }

    /** @brief Static sparse resource descriptor generated from a `#embed` byte array. */
    template<auto Uri, ResourceType Type, const auto& Bytes>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticResource {
        inline static constexpr auto kId = StaticResourceId<Uri, Type>{};
        inline static constexpr auto kUri = kId.kUri;
        inline static constexpr const char* kUriText = std::define_static_string(kUri.view());
        inline static constexpr ResourceType kType = kId.kType;
        inline static constexpr auto& kBytes = Bytes;
        inline static constexpr uint64_t kHash = kId.kHash;
        inline static constexpr uint64_t kContentHash =
            Sora::Hashing::HashByteRange(std::span<const unsigned char>{Bytes, std::size(Bytes)});

        /** @brief Return this resource as a sparse ABI descriptor. */
        [[nodiscard]] static constexpr ModuleResourceEntry Entry() noexcept {
            return ModuleResourceEntry{.semanticHash = kHash,
                                       .contentHash = kContentHash,
                                       .uri = kUriText,
                                       .uriSize = static_cast<uint32_t>(kUri.size()),
                                       .type = static_cast<uint16_t>(kType),
                                       .codec = static_cast<uint16_t>(CompressionCodec::None),
                                       .flags = static_cast<uint16_t>(ResourceFlags::None),
                                       .reserved = 0,
                                       .data = Bytes,
                                       .size = std::size(Bytes)};
        }
    };

    /** @brief Static sparse table generated from compile-time resources. */
    template<typename... Resources>
    struct StaticSparseTable {
        inline static constexpr std::array<ModuleResourceEntry, sizeof...(Resources)> kEntries{Resources::Entry()...};

        /** @brief Return a sparse table descriptor with @p priority. */
        [[nodiscard]] static constexpr SparseTable Table(int32_t priority = 0) noexcept {
            return SparseTable{.entries = kEntries.data(), .count = kEntries.size(), .priority = priority};
        }
    };

    namespace Detail {

        /** @brief Resource kind inferred from a namespace segment. */
        struct ResourceKind {
            ResourceType type = ResourceType::Unknown; /**< Semantic type. */
            std::string_view directory{};              /**< Canonical URI directory. */
            std::string_view extension{};              /**< Default filename extension. */
        };

        /** @brief Return the resource kind carried by a namespace segment. */
        [[nodiscard]] consteval ResourceKind ResourceKindOf(std::string_view name) {
            if (name == "Image") {
                return {ResourceType::Image, "image", ".png"};
            }
            if (name == "Font") {
                return {ResourceType::Font, "font", ".ttf"};
            }
            if (name == "I18n") {
                return {ResourceType::I18n, "i18n", ".json"};
            }
            if (name == "Bytecode") {
                return {ResourceType::Bytecode, "bytecode", ".bin"};
            }
            if (name == "Raw") {
                return {ResourceType::Raw, "raw", ".bin"};
            }
            if (name == "Shader") {
                return {ResourceType::Shader, "shader", ".wgsl"};
            }
            if (name == "Model") {
                return {ResourceType::Model, "model", ".mesh"};
            }
            if (name == "Material") {
                return {ResourceType::Material, "material", ".json"};
            }
            if (name == "Scene") {
                return {ResourceType::Scene, "scene", ".json"};
            }
            if (name == "UiLayout" || name == "UILayout") {
                return {ResourceType::UiLayout, "ui-layout", ".json"};
            }
            if (name == "Config") {
                return {ResourceType::Config, "config", ".ini"};
            }
            if (name == "Audio") {
                return {ResourceType::Audio, "audio", ".bin"};
            }
            if (name == "Data") {
                return {ResourceType::Data, "data", ".bin"};
            }
            return {};
        }

        /** @brief Return the single resource annotation on @p variable, if one exists. */
        [[nodiscard]] consteval std::optional<$::Resource> ResourceAnnotationOf(std::meta::info variable) {
            auto annotations = std::meta::annotations_of(variable, ^^$::Resource);
            if (annotations.size() > 1) {
                throw std::define_static_string("A Sora resource declaration can carry at most one "
                                                "Resource annotation.");
            }
            if (annotations.empty()) {
                return std::nullopt;
            }
            return std::meta::extract<$::Resource>(annotations.front());
        }

        /** @brief Find the nearest resource-kind namespace in @p variable's parent chain. */
        [[nodiscard]] consteval std::meta::info ResourceKindScopeOf(std::meta::info variable) {
            std::meta::info scope = std::meta::parent_of(variable);
            while (scope != ^^::) {
                if (std::meta::is_namespace(scope) &&
                    ResourceKindOf(std::meta::identifier_of(scope)).type != ResourceType::Unknown) {
                    return scope;
                }
                scope = std::meta::parent_of(scope);
            }
            return {};
        }

        /** @brief Return true when @p type is an unsigned byte-like scalar. */
        [[nodiscard]] consteval bool IsUnsignedByteType(std::meta::info type) {
            type = std::meta::remove_cv(std::meta::dealias(type));
            return type == ^^unsigned char;
        }

        /** @brief Return true when @p variable is a valid namespace-scoped static byte-array resource. */
        [[nodiscard]] consteval bool IsStaticResourceVariable(std::meta::info variable) {
            if (!std::meta::is_variable(variable) || !std::meta::has_identifier(variable)) {
                return false;
            }
            if (!ResourceAnnotationOf(variable).has_value() && ResourceKindScopeOf(variable) == std::meta::info{}) {
                return false;
            }
            auto type = std::meta::remove_cvref(std::meta::type_of(variable));
            if (!std::meta::is_bounded_array_type(type)) {
                return false;
            }
            return IsUnsignedByteType(std::meta::remove_extent(type));
        }

        /** @brief Infer the semantic type for a namespace-scoped static resource variable. */
        [[nodiscard]] consteval ResourceType ResourceTypeOf(std::meta::info variable) {
            if (auto annotation = ResourceAnnotationOf(variable);
                annotation.has_value() && annotation->type != ResourceType::Unknown) {
                return annotation->type;
            }
            const std::meta::info scope = ResourceKindScopeOf(variable);
            if (scope == std::meta::info{}) {
                throw std::define_static_string("A Sora resource declaration must be under a "
                                                "resource-kind namespace.");
            }
            return ResourceKindOf(std::meta::identifier_of(scope)).type;
        }

        /** @brief Infer the canonical URI for a namespace-scoped static resource variable. */
        [[nodiscard]] consteval FixedString<256> ResourceUriOf(std::meta::info variable) {
            if (auto annotation = ResourceAnnotationOf(variable); annotation.has_value() && !annotation->uri.empty()) {
                if (!ParseResourceUri(annotation->uri.view()).has_value()) {
                    throw std::define_static_string("Annotated Sora resource URI must use canonical res://path form.");
                }
                return annotation->uri;
            }

            const std::meta::info kindScope = ResourceKindScopeOf(variable);
            if (kindScope == std::meta::info{}) {
                throw std::define_static_string("A Sora resource declaration must be under a "
                                                "resource-kind namespace.");
            }

            const ResourceKind kind = ResourceKindOf(std::meta::identifier_of(kindScope));
            std::vector<std::meta::info> scopes;
            for (std::meta::info scope = std::meta::parent_of(variable); scope != kindScope;
                 scope = std::meta::parent_of(scope)) {
                if (!std::meta::is_namespace(scope)) {
                    throw std::define_static_string("Sora resource declarations must live under namespace scopes.");
                }
                scopes.push_back(scope);
            }

            std::string uri = "res://";
            uri.reserve(4 + kind.directory.size() + scopes.size() * 32 + std::meta::identifier_of(variable).size() +
                        kind.extension.size());
            uri += kind.directory;
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                uri.push_back('/');
                uri += Sora::Ascii::ToLowerKebab(std::meta::identifier_of(*it));
            }
            uri.push_back('/');
            uri += Sora::Ascii::ToLowerKebab(std::meta::identifier_of(variable));
            uri += kind.extension;
            if (!ParseResourceUri(uri).has_value()) {
                throw std::define_static_string("Inferred Sora resource URI is not canonical.");
            }
            return FixedString<256>(std::string_view{uri});
        }

    } // namespace Detail

    /**
     * @brief Normalized static resource generated from a namespace-scoped byte-array declaration.
     * @tparam Variable Reflected variable whose value is a static byte array, usually initialized with @c #embed.
     */
    template<std::meta::info Variable>
    struct StaticResourceFrom {
        static_assert(Detail::IsStaticResourceVariable(Variable),
                      "StaticResourceFrom requires a namespace-scoped unsigned byte array resource declaration.");

        inline static constexpr auto kUri = Detail::ResourceUriOf(Variable); /**< Canonical resource URI. */
        /** @brief Static-storage URI text used by ABI descriptors. */
        inline static constexpr const char* kUriText = std::define_static_string(kUri.view());
        inline static constexpr ResourceType kType = Detail::ResourceTypeOf(Variable); /**< Semantic resource type. */
        inline static constexpr auto& kBytes = [:Variable:];                           /**< Static resource bytes. */
        inline static constexpr size_t kSize = std::size(kBytes);                      /**< Payload size in bytes. */
        inline static constexpr uint64_t kHash = HashUri(kUri.view());                 /**< Semantic URI hash. */
        inline static constexpr uint64_t kContentHash =
            Sora::Hashing::HashByteRange(std::span<const unsigned char>{kBytes, kSize});

        /** @brief Return this resource as a sparse ABI descriptor. */
        [[nodiscard]] static constexpr ModuleResourceEntry Entry() noexcept {
            return ModuleResourceEntry{.semanticHash = kHash,
                                       .contentHash = kContentHash,
                                       .uri = kUriText,
                                       .uriSize = static_cast<uint32_t>(kUri.size()),
                                       .type = static_cast<uint16_t>(kType),
                                       .codec = static_cast<uint16_t>(CompressionCodec::None),
                                       .flags = static_cast<uint16_t>(ResourceFlags::None),
                                       .reserved = 0,
                                       .data = kBytes,
                                       .size = kSize};
        }
    };

    namespace Detail {

        /** @brief Append reflected static resource types found under @p scope into @p out. */
        consteval void AppendResourceTypesOf(std::vector<std::meta::info>& out, std::meta::info scope) {
            for (std::meta::info member : Sora::Meta::MembersOf(scope)) {
                if (std::meta::is_namespace(member)) {
                    AppendResourceTypesOf(out, member);
                } else if (IsStaticResourceVariable(member)) {
                    out.push_back(std::meta::substitute(^^StaticResourceFrom, {std::meta::reflect_constant(member)}));
                }
            }
        }

        /** @brief Layout metadata for one static resource inside a generated `.lpak` image. */
        struct StaticPakResourceLayout {
            uint64_t hash = 0;                     /**< Semantic URI hash. */
            ResourceType type = ResourceType::Raw; /**< Semantic resource type. */
            size_t size = 0;                       /**< Payload size in bytes. */
            size_t index = 0;                      /**< Original resource-pack index. */
            size_t uriOffset = 0;                  /**< Offset inside the lpak string section. */
            size_t dataOffset = 0;                 /**< Offset inside the lpak data section. */
            std::string_view uri{};                /**< Canonical URI. */
        };

        /** @brief Align @p value upward to @p alignment. */
        [[nodiscard]] consteval size_t AlignUp(size_t value, size_t alignment) {
            return alignment == 0 ? value : ((value + alignment - 1u) / alignment) * alignment;
        }

        /** @brief Return the content hash of @p Resource. */
        template<typename Resource>
        [[nodiscard]] consteval uint64_t ResourceHash() {
            return Resource::kContentHash;
        }

        /** @brief Return the content hash for the resource at @p index. */
        template<size_t I, typename First, typename... Rest>
        [[nodiscard]] consteval uint64_t ResourceHashByIndex(size_t index) {
            if (index == I) {
                return ResourceHash<First>();
            }
            if constexpr (sizeof...(Rest) != 0) {
                return ResourceHashByIndex<I + 1u, Rest...>(index);
            }
            throw std::define_static_string("Sora static pak resource index is out of range.");
        }

        /** @brief Copy @p Resource payload bytes into @p out. */
        template<typename Resource, size_t N>
        constexpr void CopyResourceBytes(std::array<unsigned char, N>& out, size_t offset) noexcept {
            for (size_t i = 0; i < Resource::kSize; ++i) {
                out[offset + i] = Resource::kBytes[i];
            }
        }

        /** @brief Copy the payload for resource @p index into @p out. */
        template<size_t I, typename First, typename... Rest, size_t N>
        constexpr void CopyResourceByIndex(size_t index, std::array<unsigned char, N>& out, size_t offset) {
            if (index == I) {
                CopyResourceBytes<First>(out, offset);
                return;
            }
            if constexpr (sizeof...(Rest) != 0) {
                CopyResourceByIndex<I + 1u, Rest...>(index, out, offset);
                return;
            }
            throw std::define_static_string("Sora static pak resource index is out of range.");
        }

        /** @brief Return sorted static resource layouts and section-local offsets. */
        template<typename... Resources>
        [[nodiscard]] consteval auto SortedResourceLayouts() {
            std::array<StaticPakResourceLayout, sizeof...(Resources)> layouts{
                StaticPakResourceLayout{.hash = Resources::kHash,
                                        .type = Resources::kType,
                                        .size = Resources::kSize,
                                        .index = 0,
                                        .uri = Resources::kUri.view()}...};

            for (size_t i = 0; i < layouts.size(); ++i) {
                layouts[i].index = i;
            }
            for (size_t i = 1; i < layouts.size(); ++i) {
                auto key = layouts[i];
                size_t j = i;
                while (j != 0 && key.hash < layouts[j - 1u].hash) {
                    layouts[j] = layouts[j - 1u];
                    --j;
                }
                layouts[j] = key;
            }
            for (size_t i = 1; i < layouts.size(); ++i) {
                if (layouts[i - 1u].hash == layouts[i].hash) {
                    throw std::define_static_string("Sora static pak contains duplicate resource semantic hashes.");
                }
            }

            size_t stringOffset = 0;
            size_t dataOffset = 0;
            for (auto& layout : layouts) {
                layout.uriOffset = stringOffset;
                stringOffset += layout.uri.size() + 1u;
                dataOffset = AlignUp(dataOffset, static_cast<size_t>(kDefaultDataAlignment));
                layout.dataOffset = dataOffset;
                dataOffset += layout.size;
            }
            return layouts;
        }

        /** @brief Return the byte size of the generated static `.lpak` image. */
        template<typename... Resources>
        [[nodiscard]] consteval size_t StaticPakSize() {
            constexpr auto layouts = SortedResourceLayouts<Resources...>();
            size_t stringsSize = 0;
            size_t dataSize = 0;
            for (const auto& layout : layouts) {
                stringsSize += layout.uri.size() + 1u;
                dataSize = AlignUp(dataSize, static_cast<size_t>(kDefaultDataAlignment));
                dataSize += layout.size;
            }

            constexpr size_t headerSize = Wire::SizeOf<FileHeader>();
            constexpr size_t sectionSize = Wire::SizeOf<SectionDescriptor>();
            constexpr size_t entrySize = Wire::SizeOf<ResourceEntry>();
            constexpr size_t sectionCount = 3;
            const size_t sectionTableEnd = headerSize + sectionSize * sectionCount;
            const size_t entriesOffset = AlignUp(sectionTableEnd, 8);
            const size_t stringsOffset = AlignUp(entriesOffset + entrySize * sizeof...(Resources), 8);
            const size_t dataOffset = AlignUp(stringsOffset + stringsSize, static_cast<size_t>(kDefaultDataAlignment));
            return dataOffset + dataSize;
        }

        /** @brief Build the canonical static `.lpak` image for @p Resources. */
        template<typename... Resources>
        [[nodiscard]] consteval auto BuildStaticPak() {
            constexpr auto layouts = SortedResourceLayouts<Resources...>();
            constexpr size_t fileSize = StaticPakSize<Resources...>();
            constexpr size_t headerSize = Wire::SizeOf<FileHeader>();
            constexpr size_t sectionSize = Wire::SizeOf<SectionDescriptor>();
            constexpr size_t entrySize = Wire::SizeOf<ResourceEntry>();
            constexpr size_t sectionCount = 3;
            constexpr size_t sectionTableOffset = headerSize;
            constexpr size_t entriesOffset = AlignUp(headerSize + sectionSize * sectionCount, 8);

            size_t stringsSize = 0;
            size_t dataSize = 0;
            for (const auto& layout : layouts) {
                stringsSize += layout.uri.size() + 1u;
                dataSize = AlignUp(dataSize, static_cast<size_t>(kDefaultDataAlignment));
                dataSize += layout.size;
            }

            constexpr size_t entriesSize = entrySize * sizeof...(Resources);
            const size_t stringsOffset = AlignUp(entriesOffset + entriesSize, 8);
            const size_t dataOffset = AlignUp(stringsOffset + stringsSize, static_cast<size_t>(kDefaultDataAlignment));
            std::array<unsigned char, fileSize> file{};

            size_t entryCursor = entriesOffset;
            for (const auto& layout : layouts) {
                const ResourceEntry entry{.semanticHash = layout.hash,
                                          .contentHash = ResourceHashByIndex<0, Resources...>(layout.index),
                                          .dataOffset = layout.dataOffset,
                                          .packedSize = layout.size,
                                          .unpackedSize = layout.size,
                                          .uriOffset = static_cast<uint32_t>(layout.uriOffset),
                                          .uriSize = static_cast<uint32_t>(layout.uri.size()),
                                          .type = static_cast<uint16_t>(layout.type),
                                          .codec = static_cast<uint16_t>(CompressionCodec::None),
                                          .flags = static_cast<uint16_t>(ResourceFlags::None),
                                          .reserved = 0};
                Wire::WriteUnchecked(file, entryCursor, entry);
                entryCursor += entrySize;
            }

            size_t stringCursor = stringsOffset;
            for (const auto& layout : layouts) {
                for (char c : layout.uri) {
                    file[stringCursor++] = static_cast<unsigned char>(c);
                }
                file[stringCursor++] = 0;
            }

            for (const auto& layout : layouts) {
                CopyResourceByIndex<0, Resources...>(layout.index, file, dataOffset + layout.dataOffset);
            }

            const SectionDescriptor entrySection{
                .kind = static_cast<uint32_t>(SectionKind::Entries),
                .flags = 0,
                .alignment = 8,
                .reserved = 0,
                .offset = entriesOffset,
                .size = entriesSize,
                .checksum = Sora::Hashing::HashByteRange(
                    std::span<const unsigned char>{file.data() + entriesOffset, entriesSize})};
            const SectionDescriptor stringSection{
                .kind = static_cast<uint32_t>(SectionKind::Strings),
                .flags = 0,
                .alignment = 8,
                .reserved = 0,
                .offset = stringsOffset,
                .size = stringsSize,
                .checksum = Sora::Hashing::HashByteRange(
                    std::span<const unsigned char>{file.data() + stringsOffset, stringsSize})};
            const SectionDescriptor dataSection{.kind = static_cast<uint32_t>(SectionKind::Data),
                                                .flags = 0,
                                                .alignment = static_cast<uint32_t>(kDefaultDataAlignment),
                                                .reserved = 0,
                                                .offset = dataOffset,
                                                .size = dataSize,
                                                .checksum = Sora::Hashing::HashByteRange(std::span<const unsigned char>{
                                                    file.data() + dataOffset, dataSize})};
            Wire::WriteUnchecked(file, sectionTableOffset, entrySection);
            Wire::WriteUnchecked(file, sectionTableOffset + sectionSize, stringSection);
            Wire::WriteUnchecked(file, sectionTableOffset + sectionSize * 2u, dataSection);

            FileHeader header{.magic = kLpakMagic,
                              .major = kLpakMajor,
                              .minor = kLpakMinor,
                              .headerSize = static_cast<uint16_t>(headerSize),
                              .sectionCount = static_cast<uint16_t>(sectionCount),
                              .layout = static_cast<uint16_t>(IndexLayout::Sorted),
                              .reserved0 = 0,
                              .flags = 0,
                              .fileSize = fileSize,
                              .sectionTableOffset = sectionTableOffset,
                              .resourceCount = sizeof...(Resources),
                              .headerHash = 0,
                              .fileHash = 0};
            Wire::WriteUnchecked(file, 0, header);
            header.headerHash = Sora::Hashing::HashByteRange(std::span<const unsigned char>{file.data(), headerSize});
            Wire::WriteUnchecked(file, 0, header);
            constexpr size_t fileHashOffset = Wire::OffsetOf<FileHeader, ^^FileHeader::fileHash>();
            header.fileHash = Sora::Hashing::HashByteRangeWithZeroRange(std::span<const unsigned char>{file},
                                                                        fileHashOffset, sizeof(header.fileHash));
            Wire::WriteUnchecked(file, 0, header);
            return file;
        }

    } // namespace Detail

    /** @brief Return a reflected TypeList of static resources declared below namespace @p Scope. */
    template<std::meta::info Scope>
    [[nodiscard]] consteval std::meta::info ResourceListInfoOf() {
        if (!std::meta::is_namespace(Scope)) {
            throw std::define_static_string("ResourceListInfoOf requires a namespace reflection.");
        }
        std::vector<std::meta::info> resources;
        Detail::AppendResourceTypesOf(resources, Scope);
        if (resources.empty()) {
            throw std::define_static_string("ResourceListInfoOf found no static byte-array resources.");
        }
        return std::meta::substitute(^^Sora::Traits::TypeList, resources);
    }

    /** @brief TypeList of static resources declared below namespace @p Scope. */
    template<std::meta::info Scope>
    using ResourceListOf = Sora::Meta::InfoType<ResourceListInfoOf<Scope>()>;

    /** @brief Static sparse-table builder over a @ref Sora::Traits::TypeList of resources. */
    template<typename Resources>
    struct StaticSparseTableFor;

    /** @brief Static sparse-table builder over an explicit resource list. */
    template<typename... Resources>
    struct StaticSparseTableFor<Sora::Traits::TypeList<Resources...>> : StaticSparseTable<Resources...> {};

    /** @brief Static `.lpak` image builder over a @ref Sora::Traits::TypeList of resources. */
    template<typename Resources>
    struct StaticPakImage;

    /** @brief Static `.lpak` image builder over an explicit resource list. */
    template<typename... Resources>
    struct StaticPakImage<Sora::Traits::TypeList<Resources...>> {
        static_assert(sizeof...(Resources) != 0, "StaticPakImage requires at least one resource.");

        /** @brief Canonical `.lpak` bytes generated at compile time. */
        inline static constexpr auto kBytes = Detail::BuildStaticPak<Resources...>();

        /** @brief Return this image as a module lpak block. */
        [[nodiscard]] static constexpr LpakBlock Block(int32_t priority = 0) noexcept {
            return LpakBlock{.bytes = kBytes.data(), .size = kBytes.size(), .priority = priority};
        }
    };

} // namespace Sora::Resources

/** @brief Define the canonical resource-module export function for @p ModuleObject. */
#define REGISTER_RESOURCE_MODULE(ModuleObject)                                                                         \
    extern "C" PLATFORM_EXPORT const Sora::Resources::ResourceModule* ResourceModule() noexcept {                      \
        return &(ModuleObject);                                                                                        \
    }                                                                                                                  \
                                                                                                                       \
    static_assert(sizeof(ModuleObject) == sizeof(Sora::Resources::ResourceModule),                                     \
                  "ResourceModule object must be standard-layout.");                                                   \
    static_assert(std::meta::identifier_of(^^ResourceModule) == Sora::Resources::kResourceModuleSymbol,                \
                  "ResourceModule export symbol must be named ResourceModule.");                                       \
    static_assert(                                                                                                     \
        std::convertible_to<decltype(ResourceModule), std::add_pointer_t<Sora::Resources::ResourceModuleExportFn>>,    \
        "ResourceModule object must be of type const Sora::Resources::ResourceModule.");
