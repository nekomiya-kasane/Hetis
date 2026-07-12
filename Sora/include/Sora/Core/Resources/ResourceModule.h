/**
 * @file ResourceModule.h
 * @brief Dynamic resource-module disclosure protocol and producer-side resource table helpers.
 * @ingroup Resources
 */
#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/StringUtils.h>
#include <Sora/Core/Resources/Format.h>
#include <Sora/Core/Resources/PakLayout.h>
#include <Sora/Core/Resources/ResourceAnnotation.h>
#include <Sora/Core/Resources/ResourceId.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

        /** @brief Conventional resource defaults inferred from a namespace segment. */
        struct ResourceKind {
            ResourceType type = ResourceType::Unknown; /**< Semantic type. */
            std::string_view directory{};              /**< Default URI authority or nested directory. */
            std::string_view extension{};              /**< Default filename extension. */
        };

        /** @brief Return the conventional resource defaults carried by a namespace segment. */
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

        /** @brief Active resource-tree defaults inherited while walking reflected namespaces. */
        struct ResourceScopeContext {
            FixedString<256> baseUri{};                /**< Canonical resource-tree base URI. */
            ResourceType type = ResourceType::Unknown; /**< Inherited semantic resource type. */
            FixedString<32> extension{};               /**< Inherited filename suffix. */
        };

        /** @brief Fully resolved static resource declaration. */
        struct ResolvedResourceDecl {
            FixedString<256> uri{};                    /**< Canonical resource URI. */
            ResourceType type = ResourceType::Unknown; /**< Semantic resource type. */
        };

        /** @brief Return the single resource annotation on @p entity, if one exists. */
        [[nodiscard]] consteval std::optional<$::Resource> ResourceAnnotationOf(std::meta::info entity) {
            auto annotations = Sora::$::GetAll(entity, {^^$::Resource});
            if (annotations.size() > 1) {
                throw std::define_static_string("A Sora resource declaration can carry at most one "
                                                "Resource annotation.");
            }
            if (annotations.empty()) {
                return std::nullopt;
            }
            return std::meta::extract<$::Resource>(annotations.front());
        }

        /** @brief Return true when @p text is an absolute canonical resource URI. */
        [[nodiscard]] consteval bool IsAbsoluteResourceUri(std::string_view text) {
            return ParseResourceUri(text).has_value();
        }

        /** @brief Return true when @p text is a dot-prefixed filename suffix usable for inferred resource leaves. */
        [[nodiscard]] consteval bool IsResourceExtension(std::string_view text) {
            return Sora::IsUriPathFilenameSuffix(text);
        }

        /** @brief Return @p uri without trailing separators so it can be used as a resource-tree base. */
        [[nodiscard]] consteval FixedString<256> NormalizeResourceBase(std::string_view uri) {
            auto normalized = Sora::NormalizeUriIdentityBase<256>(uri);
            if (!normalized || !IsAbsoluteResourceUri(normalized->view())) {
                throw std::define_static_string("Sora resource namespace URI must be a canonical res:// URI.");
            }
            return *normalized;
        }

        /** @brief Resolve relative resource path @p relative against canonical base URI @p base. */
        [[nodiscard]] consteval FixedString<256> JoinResourcePath(std::string_view base, std::string_view relative) {
            auto joined = Sora::JoinUriIdentityPath<256>(base, relative);
            if (!joined) {
                throw std::define_static_string("Sora resource relative URI must be a canonical slash-separated path.");
            }
            if (!IsAbsoluteResourceUri(joined->view())) {
                throw std::define_static_string("Resolved Sora resource URI is not canonical.");
            }
            return *joined;
        }

        /** @brief Resolve annotation URI text against @p context as a namespace base URI. */
        [[nodiscard]] consteval FixedString<256> ResolveNamespaceBase(ResourceScopeContext context,
                                                                      std::string_view uri) {
            if (uri.empty()) {
                return context.baseUri;
            }
            if (Sora::ParseUri(uri).has_value()) {
                return NormalizeResourceBase(uri);
            }
            return JoinResourcePath(context.baseUri.view(), uri);
        }

        /** @brief Return lower-kebab spelling for a reflected namespace identifier. */
        [[nodiscard]] consteval std::string KebabNamespaceSegment(std::meta::info scope) {
            if (!std::meta::has_identifier(scope)) {
                throw std::define_static_string("Sora resource namespaces must be named.");
            }
            std::string segment = Sora::Ascii::ToLowerKebab(std::meta::identifier_of(scope));
            if (!Sora::IsCanonicalRelativeUriIdentityPath(segment)) {
                throw std::define_static_string("Sora resource namespace name does not form a canonical path segment.");
            }
            return segment;
        }

        /** @brief Return lower-kebab filename stem for a reflected resource variable. */
        [[nodiscard]] consteval std::string KebabResourceStem(std::meta::info variable) {
            std::string stem = Sora::Ascii::ToLowerKebab(std::meta::identifier_of(variable));
            if (stem.empty() || stem.find('/') != std::string::npos ||
                !Sora::IsCanonicalRelativeUriIdentityPath(stem)) {
                throw std::define_static_string("Sora resource variable name does not form a canonical path segment.");
            }
            return stem;
        }

        /** @brief Return context for @p scope after applying namespace annotations and naming conventions. */
        [[nodiscard]] consteval ResourceScopeContext ResolveScopeContext(std::meta::info scope,
                                                                         ResourceScopeContext parent) {
            ResourceScopeContext context = parent;
            const auto annotation = ResourceAnnotationOf(scope);
            const ResourceKind kind =
                std::meta::has_identifier(scope) ? ResourceKindOf(std::meta::identifier_of(scope)) : ResourceKind{};

            if (kind.type != ResourceType::Unknown) {
                context.type = kind.type;
                context.extension = FixedString<32>{kind.extension};
            }
            if (annotation.has_value() && annotation->type != ResourceType::Unknown) {
                context.type = annotation->type;
            }
            if (annotation.has_value() && !annotation->extension.empty()) {
                if (!IsResourceExtension(annotation->extension.view())) {
                    throw std::define_static_string("Sora resource namespace extension must be a dot-prefixed suffix.");
                }
                context.extension = annotation->extension;
            }

            if (annotation.has_value() && !annotation->uri.empty()) {
                context.baseUri = ResolveNamespaceBase(parent, annotation->uri.view());
            } else if (kind.type != ResourceType::Unknown) {
                context.baseUri = parent.baseUri.empty() ? NormalizeResourceBase(std::string{"res://"} + kind.directory)
                                                         : JoinResourcePath(parent.baseUri.view(), kind.directory);
            } else if (!parent.baseUri.empty()) {
                context.baseUri = JoinResourcePath(parent.baseUri.view(), KebabNamespaceSegment(scope));
            }
            return context;
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
            if (!std::meta::is_namespace(std::meta::parent_of(variable))) {
                return false;
            }
            auto type = std::meta::remove_cvref(std::meta::type_of(variable));
            if (!std::meta::is_bounded_array_type(type)) {
                return false;
            }
            return IsUnsignedByteType(std::meta::remove_extent(type));
        }

        /** @brief Resolve the resource identity declared by @p variable under @p context. */
        [[nodiscard]] consteval std::optional<ResolvedResourceDecl> ResolveResourceDecl(std::meta::info variable,
                                                                                        ResourceScopeContext context) {
            const auto annotation = ResourceAnnotationOf(variable);
            if (!annotation.has_value() && context.baseUri.empty() && context.type == ResourceType::Unknown &&
                context.extension.empty()) {
                return std::nullopt;
            }

            ResourceType type = context.type;
            FixedString<32> extension = context.extension;
            if (annotation.has_value() && annotation->type != ResourceType::Unknown) {
                type = annotation->type;
            }
            if (annotation.has_value() && !annotation->extension.empty()) {
                if (!IsResourceExtension(annotation->extension.view())) {
                    throw std::define_static_string("Sora resource variable extension must be a dot-prefixed suffix.");
                }
                extension = annotation->extension;
            }
            if (!IsKnownResourceType(type)) {
                throw std::define_static_string("Sora resource type must be declared or inferred from namespace.");
            }

            FixedString<256> uri{};
            if (annotation.has_value() && !annotation->uri.empty()) {
                if (IsAbsoluteResourceUri(annotation->uri.view())) {
                    uri = annotation->uri;
                } else {
                    uri = JoinResourcePath(context.baseUri.view(), annotation->uri.view());
                }
            } else {
                if (context.baseUri.empty()) {
                    throw std::define_static_string("Inferred Sora resource URI requires a namespace resource base.");
                }
                if (!IsResourceExtension(extension.view())) {
                    throw std::define_static_string("Inferred Sora resource URI requires a dot-prefixed extension.");
                }
                std::string leaf = KebabResourceStem(variable);
                leaf += extension.view();
                uri = JoinResourcePath(context.baseUri.view(), leaf);
            }

            if (!IsAbsoluteResourceUri(uri.view())) {
                throw std::define_static_string("Resolved Sora resource URI is not canonical.");
            }
            return ResolvedResourceDecl{.uri = uri, .type = type};
        }

    } // namespace Detail

    /**
     * @brief Normalized static resource generated from a namespace-scoped byte-array declaration.
     * @tparam Variable Reflected variable whose value is a static byte array, usually initialized with @c #embed.
     * @tparam Uri Canonical resource URI resolved from namespace and variable annotations.
     * @tparam Type Semantic resource type resolved from namespace and variable annotations.
     */
    template<std::meta::info Variable, FixedString<256> Uri, ResourceType Type>
    struct StaticResourceFrom {
        static_assert(Detail::IsStaticResourceVariable(Variable),
                      "StaticResourceFrom requires a namespace-scoped unsigned byte array resource declaration.");
        static_assert(Detail::IsAbsoluteResourceUri(Uri.view()),
                      "StaticResourceFrom requires a canonical resource URI.");
        static_assert(IsKnownResourceType(Type), "StaticResourceFrom requires a known resource type.");

        inline static constexpr auto kUri = Uri; /**< Canonical resource URI. */
        /** @brief Static-storage URI text used by ABI descriptors. */
        inline static constexpr const char* kUriText = std::define_static_string(kUri.view());
        inline static constexpr ResourceType kType = Type;             /**< Semantic resource type. */
        inline static constexpr auto& kBytes = [:Variable:];           /**< Static resource bytes. */
        inline static constexpr size_t kSize = std::size(kBytes);      /**< Payload size in bytes. */
        inline static constexpr uint64_t kHash = HashUri(kUri.view()); /**< Semantic URI hash. */
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
        consteval void AppendResourceTypesOf(std::vector<std::meta::info>& out, std::meta::info scope,
                                             ResourceScopeContext context = {}) {
            const ResourceScopeContext current = ResolveScopeContext(scope, context);
            for (std::meta::info member : Sora::Meta::MembersOf(scope)) {
                if (std::meta::is_namespace(member)) {
                    AppendResourceTypesOf(out, member, current);
                } else if (IsStaticResourceVariable(member)) {
                    auto resolved = ResolveResourceDecl(member, current);
                    if (resolved.has_value()) {
                        out.push_back(
                            std::meta::substitute(^^StaticResourceFrom, {std::meta::reflect_constant(member),
                                                                         std::meta::reflect_constant(resolved->uri),
                                                                         std::meta::reflect_constant(resolved->type)}));
                    }
                }
            }
        }

        /** @brief Layout metadata for one static resource inside a generated `.lpak` image. */
        struct StaticPakResourceLayout {
            uint64_t hash = 0;                     /**< Semantic URI hash. */
            ResourceType type = ResourceType::Raw; /**< Semantic resource type. */
            uint64_t size = 0;                     /**< Payload size in bytes. */
            size_t index = 0;                      /**< Original resource-pack index. */
            uint32_t uriOffset = 0;                /**< Offset inside the `.lpak` string section. */
            uint32_t uriSize = 0;                  /**< URI byte length. */
            uint64_t dataOffset = 0;               /**< Offset inside the `.lpak` data section. */
            std::string_view uri{};                /**< Canonical URI. */
        };

        /** @brief Complete compile-time `.lpak` plan for a static resource pack. */
        template<size_t N>
        struct StaticPakPlan {
            std::array<StaticPakResourceLayout, N> resources{}; /**< Sorted resource layout descriptors. */
            PakLayout layout{};                                /**< Canonical file layout. */
        };

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

        /** @brief Return sorted static resource layouts and canonical file offsets. */
        template<typename... Resources>
        [[nodiscard]] consteval auto StaticPakPlanOf() {
            StaticPakPlan<sizeof...(Resources)> plan{.resources = {
                StaticPakResourceLayout{.hash = Resources::kHash,
                                        .type = Resources::kType,
                                        .size = Resources::kSize,
                                        .index = 0,
                                        .uri = Resources::kUri.view()}...}};

            for (size_t i = 0; i < plan.resources.size(); ++i) {
                plan.resources[i].index = i;
            }
            for (size_t i = 1; i < plan.resources.size(); ++i) {
                auto key = plan.resources[i];
                size_t j = i;
                while (j != 0 && key.hash < plan.resources[j - 1u].hash) {
                    plan.resources[j] = plan.resources[j - 1u];
                    --j;
                }
                plan.resources[j] = key;
            }
            for (size_t i = 1; i < plan.resources.size(); ++i) {
                if (plan.resources[i - 1u].hash == plan.resources[i].hash) {
                    throw std::define_static_string("Sora static pak contains duplicate resource semantic hashes.");
                }
            }

            uint64_t stringsSize = 0;
            uint64_t dataSize = 0;
            for (auto& resource : plan.resources) {
                auto uriOffset = PlacePakUri(stringsSize, resource.uri.size());
                auto dataOffset = PlacePakPayload(dataSize, resource.size);
                if (!uriOffset || !dataOffset) {
                    throw std::define_static_string("Sora static pak layout overflow.");
                }
                resource.uriOffset = *uriOffset;
                resource.uriSize = static_cast<uint32_t>(resource.uri.size());
                resource.dataOffset = *dataOffset;
            }

            auto layout = MakePakLayout(sizeof...(Resources), stringsSize, dataSize);
            if (!layout || layout->fileSize > std::numeric_limits<size_t>::max()) {
                throw std::define_static_string("Sora static pak file size is not representable.");
            }
            plan.layout = *layout;
            return plan;
        }

        /** @brief Return the byte size of the generated static `.lpak` image. */
        template<typename... Resources>
        [[nodiscard]] consteval size_t StaticPakSize() {
            constexpr auto plan = StaticPakPlanOf<Resources...>();
            return static_cast<size_t>(plan.layout.fileSize);
        }

        /** @brief Build the canonical static `.lpak` image for @p Resources. */
        template<typename... Resources>
        [[nodiscard]] consteval auto BuildStaticPak() {
            constexpr auto plan = StaticPakPlanOf<Resources...>();
            constexpr size_t fileSize = StaticPakSize<Resources...>();
            std::array<unsigned char, fileSize> file{};
            auto bytes = std::span<unsigned char>{file};

            for (size_t i = 0; i < plan.resources.size(); ++i) {
                const auto& resource = plan.resources[i];
                const auto entry = MakePakEntry(resource.hash, ResourceHashByIndex<0, Resources...>(resource.index),
                                                plan.layout.dataOffset + resource.dataOffset, resource.size,
                                                resource.uriOffset, resource.uriSize, resource.type);
                WritePakEntryUnchecked(bytes, plan.layout, i, entry);
            }

            for (const auto& resource : plan.resources) {
                size_t stringCursor = static_cast<size_t>(plan.layout.stringsOffset + resource.uriOffset);
                for (char c : resource.uri) {
                    file[stringCursor++] = static_cast<unsigned char>(c);
                }
                file[stringCursor++] = 0;
            }

            for (const auto& resource : plan.resources) {
                CopyResourceByIndex<0, Resources...>(
                    resource.index, file, static_cast<size_t>(plan.layout.dataOffset + resource.dataOffset));
            }

            const uint64_t entriesChecksum =
                Sora::Hashing::HashByteRange(PakEntriesRegion(std::span<const unsigned char>{file}, plan.layout));
            const uint64_t stringsChecksum =
                Sora::Hashing::HashByteRange(PakStringsRegion(std::span<const unsigned char>{file}, plan.layout));
            WritePakPreambleUnchecked(bytes, plan.layout, entriesChecksum, stringsChecksum);
            FinalizePakHeaderUnchecked(bytes, plan.layout);
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
