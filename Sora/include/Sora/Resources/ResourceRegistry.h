/**
 * @file ResourceRegistry.h
 * @brief Process-level registry for lazily loaded resource modules and layout-agnostic resource lookup.
 * @ingroup Resources
 */
#pragma once

#include <Sora/ErrorCode.h>
#include <Sora/PAL/Module.h>
#include <Sora/Resources/PakView.h>
#include <Sora/Resources/ResourceModule.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::Resources {

    /** @brief Immutable resource handle returned by @ref ResourceRegistry. */
    class ResourceBlob {
    public:
        /** @brief Construct an empty resource handle. */
        ResourceBlob() = default;

        /** @brief Construct a resource handle with explicit owner lifetime. */
        ResourceBlob(ResourceId id, std::vector<std::byte> owned, PAL::ModulePtr owner);

        /** @brief Construct a non-owning resource handle with explicit owner lifetime. */
        ResourceBlob(ResourceId id, std::span<const std::byte> bytes, PAL::ModulePtr owner);

        /** @brief Return true when this handle refers to a resource payload. */
        [[nodiscard]] explicit operator bool() const noexcept { return id_.hash != 0; }

        /** @brief Return the resource identity. */
        [[nodiscard]] const ResourceId& Id() const noexcept { return id_; }

        /** @brief Return immutable payload bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;

        /** @brief Return the module handle keeping non-owned payload memory alive. */
        [[nodiscard]] const PAL::ModulePtr& Owner() const noexcept { return owner_; }

    private:
        ResourceId id_{};
        std::string uri_{};
        std::vector<std::byte> owned_{};
        std::span<const std::byte> borrowed_{};
        PAL::ModulePtr owner_{};
    };

    /** @brief Dynamic module loading state tracked by the registry. */
    enum class ResourceModuleState : uint8_t {
        Candidate = 0, /**< Candidate path or name is known but not loaded. */
        Indexed,       /**< Module was loaded and registered into the resource index. */
        Failed,        /**< Module failed loading, symbol lookup, ABI validation, or registration. */
    };

    /** @brief Process-level resource registry merging `.lpak`, sparse, and provider-backed modules. */
    class ResourceRegistry {
    public:
        /** @brief Return the process-global resource registry. */
        [[nodiscard]] static ResourceRegistry& Default() noexcept;

        ResourceRegistry();
        ResourceRegistry(const ResourceRegistry&) = delete;
        ResourceRegistry& operator=(const ResourceRegistry&) = delete;

        /** @brief Add @p path to the dynamic module search roots. */
        void AddSearchPath(std::filesystem::path path);

        /** @brief Remove @p path from the dynamic module search roots. */
        [[nodiscard]] bool RemoveSearchPath(const std::filesystem::path& path);

        /** @brief Remove every dynamic module search root. */
        void ClearSearchPaths();

        /** @brief Return a copy of current search roots. */
        [[nodiscard]] std::vector<std::filesystem::path> SearchPaths() const;

        /** @brief Add a lazily loaded resource module candidate. */
        void AddModuleCandidate(std::string name);

        /** @brief Load and index one named resource module immediately. */
        [[nodiscard]] VoidResult LoadAndIndex(std::string_view name);

        /** @brief Load and index all pending module candidates. */
        [[nodiscard]] VoidResult LoadAndIndexAll();

        /** @brief Open a resource by semantic hash, lazily indexing pending modules if needed. */
        [[nodiscard]] Result<ResourceBlob> Open(uint64_t hash);

        /** @brief Open a resource by canonical URI, lazily indexing pending modules if needed. */
        [[nodiscard]] Result<ResourceBlob> Open(std::string_view uri);

        /** @brief Return the number of indexed dynamic resource modules. */
        [[nodiscard]] size_t ModuleCount() const;

        /** @brief Return the number of indexed physical layouts disclosed by loaded modules. */
        [[nodiscard]] size_t LayoutCount() const;

        /** @brief Return the number of visible resources after priority resolution. */
        [[nodiscard]] size_t ResourceCount() const;

        /** @brief Add an already loaded `.lpak` block to the index. Intended for module registration callbacks. */
        [[nodiscard]] VoidResult AddLpak(const LpakBlock& block, PAL::ModulePtr owner);

        /** @brief Add an already loaded sparse table to the index. Intended for module registration callbacks. */
        [[nodiscard]] VoidResult AddSparse(const SparseTable& table, PAL::ModulePtr owner);

        /** @brief Add an already loaded callback provider to the index. Intended for module registration callbacks. */
        [[nodiscard]] VoidResult AddProvider(const ResourceProvider& provider, PAL::ModulePtr owner);

    private:
        /** @brief Candidate module known to the registry but loaded only when lookup needs it. */
        struct Candidate {
            std::string Name;
            ResourceModuleState State = ResourceModuleState::Candidate;
            PAL::ModulePtr Module{};
            ErrorCode LastError = ErrorCode::Ok;
        };

        /** @brief Internal source family stored after a module registers one layout. */
        enum class SourceKind : uint8_t {
            Lpak,
            Sparse,
            Provider,
        };

        /** @brief One indexed provider of resources, owned by a loaded module. */
        struct Source {
            SourceKind Kind = SourceKind::Sparse;
            PAL::ModulePtr Owner{};
            int32_t Priority = 0;
            uint64_t Serial = 0;
            PakView Pak{};
            std::vector<ModuleResourceEntry> Entries{};
            ResourceProvider Provider{};
        };

        /** @brief One visible hash winner after priority and insertion-order resolution. */
        struct Resolved {
            uint64_t Hash = 0;
            int32_t Priority = 0;
            uint64_t Serial = 0;
            size_t SourceIndex = 0;
            size_t EntryIndex = 0;
        };
        [[nodiscard]] VoidResult RegisterModule(PAL::ModulePtr module);
        [[nodiscard]] Result<ResourceBlob> OpenIndexed(uint64_t hash) const;
        void RebuildIndex();

        mutable std::mutex mutex_;
        std::mutex registrationMutex_;
        std::vector<std::filesystem::path> searchPaths_;
        std::vector<Candidate> candidates_;
        std::vector<Source> sources_;
        std::vector<Resolved> index_;
        uint64_t insertionSerial_ = 0;
    };

} // namespace Sora::Resources
