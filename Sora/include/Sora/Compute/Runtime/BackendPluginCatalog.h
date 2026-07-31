/**
 * @file BackendPluginCatalog.h
 * @brief Transactional immutable catalog of admitted native backend plugins.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/BackendPlugin.h>
#include <Sora/Compute/Runtime/BackendPluginAdmission.h>
#include <Sora/Core/PAL/Module.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Immutable upper bound for one process-local backend plugin catalog. */
    struct BackendPluginCatalogLimits {
        size_t maximumPlugins = 256;

        /** @brief Return whether at least one plugin can be admitted. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumPlugins != 0 && maximumPlugins <= UINT32_MAX;
        }

        friend constexpr bool operator==(BackendPluginCatalogLimits, BackendPluginCatalogLimits) noexcept = default;
    };

    /** @brief Stable failure while loading or publishing one catalog generation. */
    enum class BackendPluginCatalogErrorCode : uint8_t {
        InvalidAdmission,
        InvalidModule,
        PluginLoadFailed,
        ProviderMismatch,
        DuplicateProvider,
        ImageIdentityConflict,
        InvalidExpectedEntry,
        NotFound,
        StaleEntry,
        CapacityExceeded,
        GenerationExhausted,
        OutOfMemory,
    };

    /** @brief Catalog failure preserving logical provider, content identity, revision, and negotiation diagnostics. */
    struct BackendPluginCatalogError {
        BackendPluginCatalogErrorCode code = BackendPluginCatalogErrorCode::InvalidAdmission;
        RuntimeProviderId provider{};
        RuntimeProviderId actualProvider{};
        BackendPluginImageIdentity image{};
        BackendPluginImageIdentity actualImage{};
        uint64_t expectedRevision = 0;
        uint64_t actualRevision = 0;
        BackendPluginLoadError plugin{};

        friend constexpr bool operator==(BackendPluginCatalogError, BackendPluginCatalogError) noexcept = default;
    };

    /** @brief Exact compare-and-swap token for one observed provider entry. */
    struct BackendPluginCatalogEntryVersion {
        RuntimeProviderId provider{};
        BackendPluginImageIdentity image{};
        uint64_t revision = 0;

        /** @brief Return whether this token identifies one published entry revision. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return provider.Valid() && image.Valid() && revision != 0;
        }

        friend constexpr bool operator==(BackendPluginCatalogEntryVersion,
                                         BackendPluginCatalogEntryVersion) noexcept = default;
    };

    /** @brief One admitted provider with orthogonal logical, image, and location identities. */
    class BackendPluginCatalogEntry final {
    public:
        /** @brief Return the canonical logical identity exported by the plugin root. */
        [[nodiscard]] RuntimeProviderId Provider() const noexcept { return plugin_.Provider(); }

        /** @brief Return the exact module-image identity established by host trust policy. */
        [[nodiscard]] BackendPluginImageIdentity Image() const noexcept { return admission_.image; }

        /** @brief Return the host trust fact that admitted this image. */
        [[nodiscard]] BackendPluginAdmission Admission() const noexcept { return admission_; }

        /** @brief Return the exact token required to replace or retire this observed entry. */
        [[nodiscard]] BackendPluginCatalogEntryVersion Version() const noexcept {
            return {.provider = Provider(), .image = Image(), .revision = revision_};
        }

        /** @brief Return the resolved module location for diagnostics, never as logical or content identity. */
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

        /** @brief Re-enumerate this provider into one complete owning runtime contribution. */
        [[nodiscard]] std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> Discover() const noexcept {
            return plugin_();
        }

    private:
        friend class BackendPluginCatalog;

        BackendPluginCatalogEntry(RuntimeBackendPlugin plugin, BackendPluginAdmission admission,
                                  std::filesystem::path path) noexcept
            : plugin_(std::move(plugin)), admission_(admission), path_(std::move(path)) {}

        RuntimeBackendPlugin plugin_;
        BackendPluginAdmission admission_{};
        std::filesystem::path path_{};
        uint64_t revision_ = 0;
    };

    /** @brief One immutable, internally coherent admitted-plugin generation. */
    class BackendPluginCatalogSnapshot final {
    public:
        /** @brief Return the monotonic publication generation. */
        [[nodiscard]] uint64_t Generation() const noexcept { return generation_; }

        /** @brief Return all entries in canonical provider order. */
        [[nodiscard]] std::span<const BackendPluginCatalogEntry> Entries() const noexcept { return entries_; }

        /** @brief Find one provider without allocation, or return null when absent. */
        [[nodiscard]] const BackendPluginCatalogEntry* Find(RuntimeProviderId provider) const noexcept;

    private:
        friend class BackendPluginCatalog;

        uint64_t generation_ = 0;
        std::vector<BackendPluginCatalogEntry> entries_{};
    };

    /**
     * @brief Serialized-writer catalog publishing immutable single-acquire reader generations.
     * @details Loading and ABI negotiation complete before the writer lock is acquired. Register, replace, and retire
     * publish at most one generation and preserve all earlier snapshots and module leases.
     */
    class BackendPluginCatalog final {
    public:
        /** @brief Construct an empty catalog. @throws std::invalid_argument if @p limits are invalid. */
        explicit BackendPluginCatalog(BackendPluginCatalogLimits limits = {});

        BackendPluginCatalog(const BackendPluginCatalog&) = delete;
        BackendPluginCatalog& operator=(const BackendPluginCatalog&) = delete;

        /**
         * @brief Negotiate an already authenticated module and publish one new provider.
         * @details The admitting trust provider must bind @p admission to this exact @p module handle before calling.
         */
        [[nodiscard]] std::expected<uint64_t, BackendPluginCatalogError>
        Register(PAL::ModulePtr module, BackendPluginAdmission admission) noexcept;

        /** @brief Atomically replace one provider only when its current entry equals @p expected. */
        [[nodiscard]] std::expected<uint64_t, BackendPluginCatalogError>
        Replace(BackendPluginCatalogEntryVersion expected, PAL::ModulePtr module,
                BackendPluginAdmission admission) noexcept;

        /** @brief Atomically retire one provider only when its current entry equals @p expected. */
        [[nodiscard]] std::expected<bool, BackendPluginCatalogError>
        Retire(BackendPluginCatalogEntryVersion expected) noexcept;

        /** @brief Acquire one immutable catalog generation with a single acquire operation. */
        [[nodiscard]] std::shared_ptr<const BackendPluginCatalogSnapshot> Snapshot() const noexcept;

        /** @brief Return the immutable provider-count budget. */
        [[nodiscard]] BackendPluginCatalogLimits Limits() const noexcept { return limits_; }

    private:
        [[nodiscard]] static std::expected<BackendPluginCatalogEntry, BackendPluginCatalogError>
        Prepare(PAL::ModulePtr module, BackendPluginAdmission admission) noexcept;

        void Publish(std::shared_ptr<const BackendPluginCatalogSnapshot> snapshot) noexcept;

        BackendPluginCatalogLimits limits_{};
        mutable std::mutex writer_{};
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<const BackendPluginCatalogSnapshot>> snapshot_{};
#else
        std::shared_ptr<const BackendPluginCatalogSnapshot> snapshot_{};
#endif
    };

    static_assert(std::is_trivially_copyable_v<BackendPluginCatalogLimits>);
    static_assert(std::is_trivially_copyable_v<BackendPluginCatalogError>);
    static_assert(std::is_trivially_copyable_v<BackendPluginCatalogEntryVersion>);

} // namespace Sora::Compute
