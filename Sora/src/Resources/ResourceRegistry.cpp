/**
 * @file ResourceRegistry.cpp
 * @brief Implementation of dynamic resource module registration and layout-agnostic lookup.
 * @ingroup Resources
 */

#include "Sora/Core/Resources/ResourceRegistry.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <utility>

namespace Sora::Resources {

    namespace {

        /** @brief Registration state carried through the ABI sink's opaque pointer. */
        struct RegistrationContext {
            ResourceRegistry* registry = nullptr;
            PAL::ModulePtr owner{};
        };

        /** @brief Convert a fallible operation to the ABI error-code form. */
        [[nodiscard]] constexpr ErrorCode ToErrorCode(const VoidResult& result) noexcept {
            return result ? ErrorCode::Ok : result.error();
        }

        /** @brief View a module-owned byte range. */
        [[nodiscard]] auto BytesOf(const unsigned char* data, uint64_t size) -> Result<std::span<const std::byte>> {
            if (size == 0) {
                return std::span<const std::byte>{};
            }
            if (data == nullptr || size > std::numeric_limits<size_t>::max()) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            return std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), static_cast<size_t>(size)};
        }

        /** @brief Validate a sparse or provider index entry. */
        [[nodiscard]] auto ValidateEntry(const ModuleResourceEntry& entry, bool requirePayload) -> VoidResult {
            if (!IsKnownResourceType(entry.type) || !IsKnownCompressionCodec(entry.codec) ||
                !HasOnlyKnownResourceFlags(entry.flags)) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            if (entry.uri == nullptr || entry.uriSize == 0) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }

            const std::string_view uri{entry.uri, entry.uriSize};
            auto resourceUri = ParseResourceUri(uri);
            if (!resourceUri || resourceUri->Hash() != entry.semanticHash) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }

            auto bytes = BytesOf(entry.data, entry.size);
            if (!bytes) {
                if (requirePayload) {
                    return std::unexpected(bytes.error());
                }
                if (entry.data != nullptr || entry.size != 0) {
                    return std::unexpected(bytes.error());
                }
                return {};
            }
            if (requirePayload && Sora::Hashing::HashByteRange(*bytes) != entry.contentHash) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            if (!requirePayload && entry.data != nullptr && Sora::Hashing::HashByteRange(*bytes) != entry.contentHash) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            return {};
        }

        /** @brief Add a disclosed `.lpak` block through a registry sink. */
        ErrorCode AddLpakCallback(void* registry, const LpakBlock* block) noexcept {
            auto* context = static_cast<RegistrationContext*>(registry);
            if (context == nullptr || context->registry == nullptr || block == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            return ToErrorCode(context->registry->AddLpak(*block, context->owner));
        }

        /** @brief Add a disclosed sparse table through a registry sink. */
        ErrorCode AddSparseCallback(void* registry, const SparseTable* table) noexcept {
            auto* context = static_cast<RegistrationContext*>(registry);
            if (context == nullptr || context->registry == nullptr || table == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            return ToErrorCode(context->registry->AddSparse(*table, context->owner));
        }

        /** @brief Add a disclosed callback provider through a registry sink. */
        ErrorCode AddProviderCallback(void* registry, const ResourceProvider* provider) noexcept {
            auto* context = static_cast<RegistrationContext*>(registry);
            if (context == nullptr || context->registry == nullptr || provider == nullptr) {
                return ErrorCode::InvalidArgument;
            }
            return ToErrorCode(context->registry->AddProvider(*provider, context->owner));
        }

    } // namespace

    ResourceBlob::ResourceBlob(ResourceId id, std::vector<std::byte> owned, PAL::ModulePtr owner)
        : id_(id), uri_(id.uri), owned_(std::move(owned)), owner_(std::move(owner)) {
        id_.uri = uri_;
    }

    ResourceBlob::ResourceBlob(ResourceId id, std::span<const std::byte> bytes, PAL::ModulePtr owner)
        : id_(id), uri_(id.uri), borrowed_(bytes), owner_(std::move(owner)) {
        id_.uri = uri_;
    }

    std::span<const std::byte> ResourceBlob::Bytes() const noexcept {
        if (!owned_.empty()) {
            return owned_;
        }
        return borrowed_;
    }

    ResourceRegistry& ResourceRegistry::Default() noexcept {
        static ResourceRegistry registry;
        return registry;
    }

    ResourceRegistry::ResourceRegistry() = default;

    void ResourceRegistry::AddSearchPath(std::filesystem::path path) {
        std::scoped_lock lock{mutex_};
        if (!std::ranges::contains(searchPaths_, path)) {
            searchPaths_.push_back(std::move(path));
        }
    }

    bool ResourceRegistry::RemoveSearchPath(const std::filesystem::path& path) {
        std::scoped_lock lock{mutex_};
        const auto oldSize = searchPaths_.size();
        std::erase(searchPaths_, path);
        return searchPaths_.size() != oldSize;
    }

    void ResourceRegistry::ClearSearchPaths() {
        std::scoped_lock lock{mutex_};
        searchPaths_.clear();
    }

    std::vector<std::filesystem::path> ResourceRegistry::SearchPaths() const {
        std::scoped_lock lock{mutex_};
        return searchPaths_;
    }

    void ResourceRegistry::AddModuleCandidate(std::string name) {
        if (name.empty()) {
            return;
        }
        std::scoped_lock lock{mutex_};
        const auto sameName = [&name](const Candidate& candidate) { return candidate.name == name; };
        if (!std::ranges::any_of(candidates_, sameName)) {
            candidates_.push_back(Candidate{.name = std::move(name)});
        }
    }

    VoidResult ResourceRegistry::LoadAndIndex(std::string_view name) {
        if (name.empty()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        // 1. Optimization cache: check if the module is already loaded or failed.
        std::string ownedName{name};
        std::vector<std::filesystem::path> searchPaths;
        {
            std::scoped_lock lock{mutex_};
            searchPaths = searchPaths_;
            for (const auto& candidate : candidates_) {
                if (candidate.name != name) {
                    continue;
                }
                if (candidate.state == ResourceModuleState::Indexed) {
                    return {};
                }
                if (candidate.state == ResourceModuleState::Failed) {
                    return std::unexpected(candidate.lastError);
                }
            }
        }

        // 2. Load the module and register its disclosed layouts.
        std::array<std::string_view, 1> names{ownedName};
        PAL::ModuleLoadOptions options;
        options.searchPaths = searchPaths;
        auto loaded = PAL::ModuleLoader::Default().Load(names, options);
        if (!loaded) {
            std::scoped_lock lock{mutex_};
            auto it = std::ranges::find(candidates_, name, &Candidate::name);
            if (it == candidates_.end()) {
                candidates_.push_back(Candidate{.name = std::move(ownedName)});
                it = std::prev(candidates_.end());
            }
            it->state = ResourceModuleState::Failed;
            it->lastError = ErrorCode::ModuleLoadFailed;
            return std::unexpected(ErrorCode::ModuleLoadFailed);
        }

        auto registered = RegisterModule(*loaded);
        std::scoped_lock lock{mutex_};
        auto it = std::ranges::find(candidates_, name, &Candidate::name);
        if (it == candidates_.end()) {
            candidates_.push_back(Candidate{.name = std::move(ownedName)});
            it = std::prev(candidates_.end());
        }
        it->module = *loaded;
        it->state = registered ? ResourceModuleState::Indexed : ResourceModuleState::Failed;
        it->lastError = registered ? ErrorCode::Ok : registered.error();
        return registered;
    }

    VoidResult ResourceRegistry::LoadAndIndexAll() {
        std::vector<std::string> pending;
        {
            std::scoped_lock lock{mutex_};
            for (const auto& candidate : candidates_) {
                if (candidate.state == ResourceModuleState::Candidate) {
                    pending.push_back(candidate.name);
                }
            }
        }

        ErrorCode firstError = ErrorCode::Ok;
        for (const auto& name : pending) {
            auto indexed = LoadAndIndex(name);
            if (!indexed && firstError == ErrorCode::Ok) {
                firstError = indexed.error();
            }
        }
        if (firstError != ErrorCode::Ok) {
            return std::unexpected(firstError);
        }
        return {};
    }

    Result<ResourceBlob> ResourceRegistry::Open(uint64_t hash) {
        if (auto found = OpenIndexed(hash)) {
            return found;
        }

        auto loaded = LoadAndIndexAll();
        if (auto found = OpenIndexed(hash)) {
            return found;
        }
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        return std::unexpected(ErrorCode::ResourceNotFound);
    }

    Result<ResourceBlob> ResourceRegistry::Open(std::string_view uri) {
        auto resourceUri = ParseResourceUri(uri);
        if (!resourceUri) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        return Open(resourceUri->Hash());
    }

    VoidResult ResourceRegistry::MountPak(std::string name, std::vector<std::byte> bytes, int32_t priority) {
        if (name.empty()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::scoped_lock lock{mutex_};
        if (ContainsSourceNameLocked(name)) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        auto pak = PakView::Open(bytes);
        if (!pak) {
            return std::unexpected(pak.error());
        }
        sources_.push_back(Source{.kind = SourceKind::Lpak,
                                  .name = std::move(name),
                                  .priority = priority,
                                  .serial = ++insertionSerial_,
                                  .ownedBytes = std::move(bytes),
                                  .pakView = std::move(*pak)});
        RebuildIndex();
        return {};
    }

    VoidResult ResourceRegistry::MountEmbeddedPak(std::string name, std::span<const std::byte> bytes,
                                                  int32_t priority) {
        if (name.empty()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::scoped_lock lock{mutex_};
        if (ContainsSourceNameLocked(name)) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        auto pak = PakView::Open(bytes);
        if (!pak) {
            return std::unexpected(pak.error());
        }
        sources_.push_back(Source{.kind = SourceKind::Lpak,
                                  .name = std::move(name),
                                  .priority = priority,
                                  .serial = ++insertionSerial_,
                                  .pakView = std::move(*pak)});
        RebuildIndex();
        return {};
    }

    size_t ResourceRegistry::ModuleCount() const {
        std::scoped_lock lock{mutex_};
        return static_cast<size_t>(std::ranges::count(candidates_, ResourceModuleState::Indexed, &Candidate::state));
    }

    size_t ResourceRegistry::LayoutCount() const {
        std::scoped_lock lock{mutex_};
        return sources_.size();
    }

    size_t ResourceRegistry::ResourceCount() const {
        std::scoped_lock lock{mutex_};
        return index_.size();
    }

    VoidResult ResourceRegistry::AddLpak(const LpakBlock& block, PAL::ModulePtr owner) {
        if (!IsCompatible(block.header, sizeof(LpakBlock))) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        auto bytes = BytesOf(block.bytes, block.size);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto pak = PakView::Open(*bytes);
        if (!pak) {
            return std::unexpected(pak.error());
        }

        std::scoped_lock lock{mutex_};
        sources_.push_back(Source{.kind = SourceKind::Lpak,
                                  .owner = std::move(owner),
                                  .priority = block.priority,
                                  .serial = ++insertionSerial_,
                                  .pakView = std::move(*pak)});
        RebuildIndex();
        return {};
    }

    VoidResult ResourceRegistry::AddSparse(const SparseTable& table, PAL::ModulePtr owner) {
        if (!IsCompatible(table.header, sizeof(SparseTable)) || (table.count != 0 && table.entries == nullptr) ||
            table.count > std::numeric_limits<size_t>::max()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::vector<ModuleResourceEntry> entries;
        entries.reserve(static_cast<size_t>(table.count));
        for (uint64_t i = 0; i < table.count; ++i) {
            if (auto valid = ValidateEntry(table.entries[i], true); !valid) {
                return std::unexpected(valid.error());
            }
            entries.push_back(table.entries[i]);
        }

        std::scoped_lock lock{mutex_};
        sources_.push_back(Source{.kind = SourceKind::Sparse,
                                  .owner = std::move(owner),
                                  .priority = table.priority,
                                  .serial = ++insertionSerial_,
                                  .entries = std::move(entries)});
        RebuildIndex();
        return {};
    }

    VoidResult ResourceRegistry::AddProvider(const ResourceProvider& provider, PAL::ModulePtr owner) {
        if (!IsCompatible(provider.header, sizeof(ResourceProvider)) || provider.open == nullptr ||
            (provider.count != 0 && provider.entries == nullptr) ||
            provider.count > std::numeric_limits<size_t>::max()) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::vector<ModuleResourceEntry> entries;
        entries.reserve(static_cast<size_t>(provider.count));
        for (uint64_t i = 0; i < provider.count; ++i) {
            if (auto valid = ValidateEntry(provider.entries[i], false); !valid) {
                return std::unexpected(valid.error());
            }
            entries.push_back(provider.entries[i]);
        }

        std::scoped_lock lock{mutex_};
        sources_.push_back(Source{.kind = SourceKind::Provider,
                                  .owner = std::move(owner),
                                  .priority = provider.priority,
                                  .serial = ++insertionSerial_,
                                  .entries = std::move(entries),
                                  .provider = provider});
        RebuildIndex();
        return {};
    }

    VoidResult ResourceRegistry::RegisterModule(PAL::ModulePtr module) {
        if (!module) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }

        std::scoped_lock registrationLock{registrationMutex_};
        size_t sourceCheckpoint = 0;
        {
            std::scoped_lock lock{mutex_};
            sourceCheckpoint = sources_.size();
        }

        // 1. Find the module's exported resource descriptor function (`ResourceModule`).
        auto* getModule = module->TryFindFunction<ResourceModuleExportFn>(kResourceModuleSymbol);
        if (getModule == nullptr) {
            return std::unexpected(ErrorCode::ModuleLoadFailed);
        }

        // 2. Call the module's `ResourceModule` function to get its descriptor of type
        // `Sora::Resources::ResourceModule` and validate it.
        const ResourceModule* descriptor = getModule();
        if (descriptor == nullptr || !IsCompatible(descriptor->header, sizeof(ResourceModule)) ||
            descriptor->registerModule == nullptr) {
            return std::unexpected(ErrorCode::ModuleLoadFailed);
        }

        // 3. Create a registry sink and call the module's `registerModule` function to let it disclose its layouts.
        RegistrationContext context{.registry = this, .owner = std::move(module)};
        RegistrySink sink{.registry = &context,
                          .addLpak = AddLpakCallback,
                          .addSparse = AddSparseCallback,
                          .addProvider = AddProviderCallback};
        const ErrorCode result = descriptor->registerModule(descriptor, &sink);
        if (result != ErrorCode::Ok) {
            std::scoped_lock lock{mutex_};
            sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(sourceCheckpoint), sources_.end());
            RebuildIndex();
            return std::unexpected(result);
        }
        return {};
    }

    Result<ResourceBlob> ResourceRegistry::OpenIndexed(uint64_t hash) const {
        std::scoped_lock lock{mutex_};
        const auto it = std::ranges::lower_bound(index_, hash, {}, &Resolved::hash);
        if (it == index_.end() || it->hash != hash) {
            return std::unexpected(ErrorCode::ResourceNotFound);
        }

        switch (const Source& source = sources_[it->sourceIndex]; source.kind) {
            case SourceKind::Lpak: {
                const ResourceEntry& entry = source.pakView.Entries()[it->entryIndex];
                auto uri = source.pakView.UriOf(entry);
                auto bytes = source.pakView.DataOf(entry);
                if (!uri) {
                    return std::unexpected(uri.error());
                }
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                return ResourceBlob{
                    ResourceId{.hash = entry.semanticHash, .type = static_cast<ResourceType>(entry.type), .uri = *uri},
                    *bytes, source.owner};
            }
            case SourceKind::Sparse: {
                const ModuleResourceEntry& entry = source.entries[it->entryIndex];
                auto bytes = BytesOf(entry.data, entry.size);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                return ResourceBlob{ResourceId{.hash = entry.semanticHash,
                                               .type = static_cast<ResourceType>(entry.type),
                                               .uri = std::string_view{entry.uri, entry.uriSize}},
                                    *bytes, source.owner};
            }
            case SourceKind::Provider: {
                const ModuleResourceEntry& entry = source.entries[it->entryIndex];
                ResourcePayload payload{};
                const ErrorCode opened = source.provider.open(source.provider.context, entry.semanticHash, &payload);
                if (opened != ErrorCode::Ok) {
                    return std::unexpected(opened);
                }
                auto bytes = BytesOf(payload.data, payload.size);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                return ResourceBlob{ResourceId{.hash = entry.semanticHash,
                                               .type = static_cast<ResourceType>(entry.type),
                                               .uri = std::string_view{entry.uri, entry.uriSize}},
                                    *bytes, source.owner};
            }
        }
        return std::unexpected(ErrorCode::InvalidState);
    }

    bool ResourceRegistry::ContainsSourceNameLocked(std::string_view name) const noexcept {
        return !name.empty() && std::ranges::any_of(sources_, [name](const Source& source) {
                   return source.name == name;
               });
    }

    void ResourceRegistry::RebuildIndex() {
        index_.clear();
        for (size_t sourceIndex = 0; sourceIndex < sources_.size(); ++sourceIndex) {
            const Source& source = sources_[sourceIndex];
            if (source.kind == SourceKind::Lpak) {
                auto entries = source.pakView.Entries();
                for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
                    index_.push_back(Resolved{.hash = entries[entryIndex].semanticHash,
                                              .priority = source.priority,
                                              .serial = source.serial,
                                              .sourceIndex = sourceIndex,
                                              .entryIndex = entryIndex});
                }
            } else {
                for (size_t entryIndex = 0; entryIndex < source.entries.size(); ++entryIndex) {
                    index_.push_back(Resolved{.hash = source.entries[entryIndex].semanticHash,
                                              .priority = source.priority,
                                              .serial = source.serial,
                                              .sourceIndex = sourceIndex,
                                              .entryIndex = entryIndex});
                }
            }
        }

        std::ranges::sort(index_);
        const auto [first, last] = std::ranges::unique(index_, {}, &Resolved::hash);
        index_.erase(first, last);
    }

} // namespace Sora::Resources
