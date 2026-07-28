/**
 * @file Settings.h
 * @brief Settings resource-domain specialization backed by simdjson compile-time JSON parsing.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/Resources/ResourceDomain.h>

#include <simdjson.h>

namespace Sora {

    namespace Resources {

        inline namespace Settings {

            /** @brief Resource-domain specialization for settings payloads. */
            struct SettingsDomain final {
                inline static constexpr ResourceDomainDescriptor kDescriptor =
                    MakeResourceDomainDescriptor("settings", ResourceType::Settings, RuntimeEntryPolicy::DeclaredOnly);
            };

            /**
             * @brief Static settings document parsed into a C++ value at compile time and exposed as resource bytes.
             */
            template<auto Uri, const auto& JsonBytes>
                requires Sora::Concept::FixedStringLike<decltype(Uri)>
            struct StaticSetting : StaticDomainResource<SettingsDomain, Uri, JsonBytes> {
                using Base = StaticDomainResource<SettingsDomain, Uri, JsonBytes>;

                inline static constexpr auto kId = StaticDomainResourceId<SettingsDomain, Uri>{};
                inline static constexpr auto kSettingsUri = Base::kDomainUri;
                inline static constexpr auto kResourceUri = Base::kResourceUri;
                inline static constexpr auto& kBytes = JsonBytes;
                inline static constexpr auto kValue = simdjson::compile_time::parse_json<JsonBytes>();
                using Value = decltype(kValue);
                using Resource = typename Base::Resource;

                /** @brief Return this setting as a sparse ABI descriptor. */
                [[nodiscard]] static constexpr ModuleResourceEntry Entry() noexcept { return Resource::Entry(); }
            };

            /** @brief Runtime provider for declared settings payload overrides. */
            using RuntimeSettingsProvider = RuntimeDomainResourceProvider<SettingsDomain>;

        } // namespace Settings

    } // namespace Resources

    namespace Settings {

        using SettingsDomain = Sora::Resources::SettingsDomain;
        using RuntimeSettingsProvider = Sora::Resources::RuntimeSettingsProvider;

    } // namespace Settings

} // namespace Sora