/**
 * @file SystemArtifactStore.cpp
 * @brief Resolve automatic platform artifact-cache namespaces.
 * @ingroup Compute
 */

#include <Sora/Compute/SystemArtifactStore.h>

#include <Sora/Core/PAL/Environment.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr ArtifactStoreError InvalidConfiguration() noexcept {
            return {ArtifactStoreErrorCode::InvalidConfiguration, 0};
        }

        [[nodiscard]] ArtifactStoreError EnvironmentFailure(ErrorCode error) noexcept {
            return {ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error)};
        }

        [[nodiscard]] std::expected<std::optional<std::string>, ArtifactStoreError>
        ReadEnvironment(std::string_view name) noexcept {
            auto value = PAL::ReadEnvironmentVariable(name);
            if (!value) {
                return std::unexpected(EnvironmentFailure(value.error()));
            }
            if (!*value || (*value)->empty()) {
                return std::optional<std::string>{};
            }
            return std::optional<std::string>{std::move(**value)};
        }

        [[nodiscard]] std::expected<std::filesystem::path, ArtifactStoreError>
        NativePath(std::string_view utf8) noexcept {
            try {
                if constexpr (Platform::kIsWindows) {
                    auto wide = Unicode::Utf8ToWide(utf8);
                    if (!wide) {
                        return std::unexpected(InvalidConfiguration());
                    }
                    return std::filesystem::path{std::move(*wide)};
                } else {
                    return std::filesystem::path{utf8};
                }
            } catch (const std::bad_alloc&) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
            }
        }

        [[nodiscard]] std::expected<std::optional<std::filesystem::path>, ArtifactStoreError>
        EnvironmentPath(std::string_view name) noexcept {
            auto value = ReadEnvironment(name);
            if (!value || !*value) {
                if (!value) {
                    return std::unexpected(value.error());
                }
                return std::optional<std::filesystem::path>{};
            }
            auto path = NativePath(**value);
            if (!path) {
                return std::unexpected(path.error());
            }
            return std::optional<std::filesystem::path>{std::move(*path)};
        }

        [[nodiscard]] std::expected<std::filesystem::path, ArtifactStoreError> PlatformCacheBase() noexcept {
            if constexpr (Platform::kIsWindows) {
                auto local = EnvironmentPath("LOCALAPPDATA");
                if (!local) {
                    return std::unexpected(local.error());
                }
                if (*local && (*local)->is_absolute()) {
                    return std::move(**local);
                }
            } else if constexpr (Platform::kIsMacOS) {
                auto home = EnvironmentPath("HOME");
                if (!home) {
                    return std::unexpected(home.error());
                }
                if (*home && (*home)->is_absolute()) {
                    return std::move(**home) / "Library" / "Caches";
                }
            } else if constexpr (Platform::kIsPosixLike) {
                auto xdg = EnvironmentPath("XDG_CACHE_HOME");
                if (!xdg) {
                    return std::unexpected(xdg.error());
                }
                if (*xdg && (*xdg)->is_absolute()) {
                    return std::move(**xdg);
                }
                auto home = EnvironmentPath("HOME");
                if (!home) {
                    return std::unexpected(home.error());
                }
                if (*home && (*home)->is_absolute()) {
                    return std::move(**home) / ".cache";
                }
            }

            std::error_code error;
            auto temporary = std::filesystem::temp_directory_path(error);
            if (error || temporary.empty()) {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
            }
            return temporary;
        }

        [[nodiscard]] std::expected<std::filesystem::path, ArtifactStoreError> SystemArtifactStoreRoot() noexcept {
            auto override = EnvironmentPath(kSystemArtifactStoreRootEnvironment);
            if (!override) {
                return std::unexpected(override.error());
            }
            if (*override) {
                if (!(*override)->is_absolute()) {
                    return std::unexpected(InvalidConfiguration());
                }
                return std::move(**override);
            }
            auto base = PlatformCacheBase();
            if (!base) {
                return std::unexpected(base.error());
            }
            try {
                return std::move(*base) / "Sora" / "Compute" / "Artifacts";
            } catch (const std::bad_alloc&) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
            } catch (...) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
            }
        }

    } // namespace

    std::expected<std::optional<ArtifactStoreConfig>, ArtifactStoreError>
    ResolveSystemArtifactStoreConfig(SystemArtifactStorePolicy policy) noexcept {
        if (!policy.Valid()) {
            return std::unexpected(InvalidConfiguration());
        }
        if (policy.mode == SystemArtifactStoreMode::Disabled) {
            return std::optional<ArtifactStoreConfig>{};
        }
        auto root = SystemArtifactStoreRoot();
        if (!root) {
            return std::unexpected(root.error());
        }
        try {
            return std::optional<ArtifactStoreConfig>{ArtifactStoreConfig{
                .root = std::move(*root),
                .maximumPayloadBytes = policy.maximumPayloadBytes,
                .maximumStorageBytes = policy.maximumStorageBytes,
                .maximumEntryCount = policy.maximumEntryCount,
                .reconciliationPublicationInterval = policy.reconciliationPublicationInterval,
                .reconciliationByteInterval = policy.reconciliationByteInterval,
                .durableWrites = policy.durableWrites,
            }};
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

} // namespace Sora::Compute
