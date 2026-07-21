/**
 * @file Settings.h
 * @brief Compile-time and runtime settings resources backed by simdjson and the Sora resource registry.
 * @ingroup Resources
 */
#pragma once

#include <Sora/ErrorCode.h>
#include <Sora/Core/Resources/ResourceModule.h>

#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Sora::Resources {

    /** @brief Canonical resource-tree prefix used by @c settings:// aliases. */
    inline constexpr std::string_view kSettingsResourcePrefix = "res://settings";

    /** @brief Return true when @p uri is a canonical @c settings://module[/entry] identity URI. */
    [[nodiscard]] constexpr bool IsSettingsUri(std::string_view uri) noexcept {
        const auto parsed = Sora::ParseUri(uri);
        if (!parsed || parsed->scheme != "settings" || !parsed->hasAuthority || parsed->authority.empty() ||
            parsed->hasQuery || parsed->hasFragment || uri.contains('\\')) {
            return false;
        }
        if (!Sora::IsCanonicalRelativeUriIdentityPath(parsed->authority)) {
            return false;
        }
        std::string_view path = parsed->path;
        if (path.starts_with('/')) {
            path.remove_prefix(1);
        }
        return path.empty() || Sora::IsCanonicalRelativeUriIdentityPath(path);
    }

    /** @brief Return @p uri converted from @c settings://module/entry to @c res://settings/module/entry. */
    template<size_t Capacity = 256>
    [[nodiscard]] constexpr auto SettingsResourceUriText(std::string_view uri) noexcept
        -> std::expected<FixedString<Capacity>, ErrorCode> {
        const auto parsed = Sora::ParseUri(uri);
        if (!parsed || !IsSettingsUri(uri)) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        FixedString<Capacity> out{};
        if (kSettingsResourcePrefix.size() + 1u + parsed->authority.size() + parsed->path.size() > out.capacity()) {
            return std::unexpected(ErrorCode::OutOfRange);
        }
        out.append(kSettingsResourcePrefix);
        out.push_back('/');
        out.append(parsed->authority);
        out.append(parsed->path);
        if (!ParseResourceUri(out.view()).has_value()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        return out;
    }

    /** @brief Normalize a canonical resource URI or @c settings:// alias into resource identity text. */
    [[nodiscard]] inline auto NormalizeResourceOrSettingsUri(std::string_view uri) -> Result<std::string> {
        if (auto normalized = NormalizeResourceUri(uri)) {
            return normalized;
        }
        auto settings = SettingsResourceUriText(uri);
        if (!settings) {
            return std::unexpected(settings.error());
        }
        return std::string(settings->view());
    }

    /** @brief Compile-time conversion from a settings URI literal to its canonical resource URI. */
    template<auto Uri>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    [[nodiscard]] consteval FixedString<256> StaticSettingsResourceUri() {
        auto resourceUri = SettingsResourceUriText(Uri.view());
        if (!resourceUri) {
            throw std::define_static_string("Sora settings URI must be settings://module[/entry] and canonical.");
        }
        return *resourceUri;
    }

    /** @brief Build a canonical settings URI from module and entry path fragments. */
    template<auto Module, auto Entry>
        requires Sora::Concept::FixedStringLike<decltype(Module)> && Sora::Concept::FixedStringLike<decltype(Entry)>
    [[nodiscard]] consteval FixedString<256> MakeSettingsUri() {
        if (!Sora::IsCanonicalRelativeUriIdentityPath(Module.view()) ||
            !Sora::IsCanonicalRelativeUriIdentityPath(Entry.view())) {
            throw std::define_static_string("Sora settings module and entry must be canonical relative URI paths.");
        }
        FixedString<256> out{};
        out.append("settings://");
        out.append(Module.view());
        out.push_back('/');
        out.append(Entry.view());
        return out;
    }

    /** @brief Compile-time settings identity carrier that precomputes both alias and resource hashes. */
    template<auto Uri>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticSettingsId {
        inline static constexpr auto kSettingsUri = Uri;
        inline static constexpr auto kResourceUri = StaticSettingsResourceUri<Uri>();
        inline static constexpr uint64_t kHash = HashUri(kResourceUri.view());

        [[nodiscard]] static constexpr ResourceId Runtime() noexcept {
            return ResourceId{.hash = kHash, .type = ResourceType::Settings, .uri = kResourceUri.view()};
        }
    };

    /** @brief Static settings document parsed into a C++ value at compile time and exposed as resource bytes. */
    template<auto Uri, const auto& JsonBytes>
        requires Sora::Concept::FixedStringLike<decltype(Uri)>
    struct StaticSetting {
        inline static constexpr auto kId = StaticSettingsId<Uri>{};
        inline static constexpr auto kSettingsUri = kId.kSettingsUri;
        inline static constexpr auto kResourceUri = kId.kResourceUri;
        inline static constexpr auto& kBytes = JsonBytes;
        inline static constexpr auto kValue = simdjson::compile_time::parse_json<JsonBytes>();
        using Value = decltype(kValue);
        using Resource = StaticResource<kResourceUri, ResourceType::Settings, JsonBytes>;

        /** @brief Return this setting as a sparse ABI descriptor. */
        [[nodiscard]] static constexpr ModuleResourceEntry Entry() noexcept { return Resource::Entry(); }
    };

    /** @brief Convenience alias for a static setting addressed by separate module and entry fragments. */
    template<auto Module, auto Entry, const auto& JsonBytes>
    using StaticSettingFor = StaticSetting<MakeSettingsUri<Module, Entry>(), JsonBytes>;

    /** @brief Provider-side table for runtime-updatable settings payloads indexed by canonical settings URIs. */
    class RuntimeSettingsProvider {
    public:
        RuntimeSettingsProvider() = default;
        RuntimeSettingsProvider(const RuntimeSettingsProvider&) = delete;
        RuntimeSettingsProvider& operator=(const RuntimeSettingsProvider&) = delete;

        /** @brief Add or replace one runtime settings payload before exposing @ref Provider to the registry. */
        [[nodiscard]] VoidResult Set(std::string_view uri, std::span<const std::byte> bytes) {
            auto normalized = NormalizeResourceOrSettingsUri(uri);
            if (!normalized) {
                return std::unexpected(normalized.error());
            }
            std::unique_lock lock{mutex_};
            const uint64_t hash = HashUri(*normalized);
            for (Record& record : records_) {
                if (record.hash == hash) {
                    record.bytes.assign(bytes.begin(), bytes.end());
                    RebuildEntriesLocked();
                    return {};
                }
            }
            records_.push_back(Record{.uri = std::move(*normalized), .hash = hash});
            records_.back().bytes.assign(bytes.begin(), bytes.end());
            RebuildEntriesLocked();
            return {};
        }

        /** @brief Add or replace one runtime settings payload from UTF-8 JSON text. */
        [[nodiscard]] VoidResult SetText(std::string_view uri, std::string_view text) {
            return Set(uri, std::as_bytes(std::span{text.data(), text.size()}));
        }

        /** @brief Return a provider descriptor suitable for @ref ResourceRegistry::AddProvider. */
        [[nodiscard]] ResourceProvider Provider(int32_t priority = 0) noexcept {
            return ResourceProvider{.context = this,
                                    .entries = entries_.data(),
                                    .count = entries_.size(),
                                    .priority = priority,
                                    .open = Open};
        }

    private:
        struct Record {
            std::string uri{};
            uint64_t hash = 0;
            std::vector<std::byte> bytes{};
        };

        void RebuildEntriesLocked() {
            entries_.clear();
            entries_.reserve(records_.size());
            for (const Record& record : records_) {
                entries_.push_back(ModuleResourceEntry{.semanticHash = record.hash,
                                                       .contentHash = Sora::Hashing::HashByteRange(record.bytes),
                                                       .uri = record.uri.data(),
                                                       .uriSize = static_cast<uint32_t>(record.uri.size()),
                                                       .type = static_cast<uint16_t>(ResourceType::Settings)});
            }
        }

        static ErrorCode Open(void* context, uint64_t hash, ResourcePayload* out) noexcept {
            if (context == nullptr || out == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            auto* self = static_cast<RuntimeSettingsProvider*>(context);
            static thread_local std::vector<std::byte> payload;
            std::shared_lock lock{self->mutex_};
            const auto it = std::ranges::find(self->records_, hash, &Record::hash);
            if (it == self->records_.end()) {
                return ErrorCode::ResourceNotFound;
            }
            payload = it->bytes;
            out->data = reinterpret_cast<const unsigned char*>(payload.data());
            out->size = payload.size();
            return ErrorCode::Ok;
        }

        mutable std::shared_mutex mutex_{};
        std::vector<Record> records_{};
        std::vector<ModuleResourceEntry> entries_{};
    };

} // namespace Sora::Resources
