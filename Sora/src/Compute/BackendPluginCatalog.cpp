/**
 * @file BackendPluginCatalog.cpp
 * @brief Transactional publication and immutable lookup for admitted backend plugins.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/BackendPluginCatalog.h>

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr auto ProviderKey(const BackendPluginCatalogEntry& entry) noexcept {
            const RuntimeProviderId provider = entry.Provider();
            return std::pair{provider.low, provider.high};
        }

        [[nodiscard]] constexpr BackendPluginCatalogError
        Failure(BackendPluginCatalogErrorCode code, RuntimeProviderId provider = {},
                BackendPluginImageIdentity image = {}, uint64_t expectedRevision = 0, uint64_t actualRevision = 0,
                RuntimeProviderId actualProvider = {}, BackendPluginImageIdentity actualImage = {}) noexcept {
            return {.code = code,
                    .provider = provider,
                    .actualProvider = actualProvider,
                    .image = image,
                    .actualImage = actualImage,
                    .expectedRevision = expectedRevision,
                    .actualRevision = actualRevision};
        }

        [[nodiscard]] auto FindProvider(std::vector<BackendPluginCatalogEntry>& entries,
                                        RuntimeProviderId provider) noexcept {
            return std::ranges::lower_bound(entries, std::pair{provider.low, provider.high}, {}, ProviderKey);
        }

        [[nodiscard]] auto FindProvider(const std::vector<BackendPluginCatalogEntry>& entries,
                                        RuntimeProviderId provider) noexcept {
            return std::ranges::lower_bound(entries, std::pair{provider.low, provider.high}, {}, ProviderKey);
        }

        [[nodiscard]] const BackendPluginCatalogEntry* FindImage(const std::vector<BackendPluginCatalogEntry>& entries,
                                                                 BackendPluginImageIdentity image,
                                                                 RuntimeProviderId except = {}) noexcept {
            const auto found = std::ranges::find_if(entries, [image, except](const BackendPluginCatalogEntry& entry) {
                return entry.Provider() != except && entry.Image() == image;
            });
            return found != entries.end() ? std::addressof(*found) : nullptr;
        }

    } // namespace

    const BackendPluginCatalogEntry* BackendPluginCatalogSnapshot::Find(RuntimeProviderId provider) const noexcept {
        const auto found = FindProvider(entries_, provider);
        return found != entries_.end() && found->Provider() == provider ? std::addressof(*found) : nullptr;
    }

    BackendPluginCatalog::BackendPluginCatalog(BackendPluginCatalogLimits limits) : limits_(limits) {
        if (!limits_.Valid()) {
            throw std::invalid_argument{"BackendPluginCatalog limits must admit at least one provider"};
        }
        Publish(std::make_shared<BackendPluginCatalogSnapshot>());
    }

    std::shared_ptr<const BackendPluginCatalogSnapshot> BackendPluginCatalog::Snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        return snapshot_.load(std::memory_order_acquire);
#else
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
#endif
    }

    void BackendPluginCatalog::Publish(std::shared_ptr<const BackendPluginCatalogSnapshot> snapshot) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
        snapshot_.store(std::move(snapshot), std::memory_order_release);
#else
        std::atomic_store_explicit(&snapshot_, std::move(snapshot), std::memory_order_release);
#endif
    }

    std::expected<BackendPluginCatalogEntry, BackendPluginCatalogError>
    BackendPluginCatalog::Prepare(PAL::ModulePtr module, BackendPluginAdmission admission) noexcept {
        if (!admission.Valid()) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::InvalidAdmission, {}, admission.image));
        }
        if (module == nullptr || !*module || module->Path().empty()) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::InvalidModule, {}, admission.image));
        }
        std::filesystem::path path;
        try {
            path = module->Path().lexically_normal();
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::OutOfMemory, {}, admission.image));
        }
        auto plugin = LoadRuntimeBackendPlugin(std::move(module));
        if (!plugin) {
            BackendPluginCatalogError error =
                Failure(BackendPluginCatalogErrorCode::PluginLoadFailed, {}, admission.image);
            error.plugin = plugin.error();
            return std::unexpected(error);
        }
        return BackendPluginCatalogEntry{std::move(*plugin), admission, std::move(path)};
    }

    std::expected<uint64_t, BackendPluginCatalogError>
    BackendPluginCatalog::Register(PAL::ModulePtr module, BackendPluginAdmission admission) noexcept {
        auto prepared = Prepare(std::move(module), admission);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        const RuntimeProviderId provider = prepared->Provider();
        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            if (current->generation_ == std::numeric_limits<uint64_t>::max()) {
                return std::unexpected(
                    Failure(BackendPluginCatalogErrorCode::GenerationExhausted, provider, admission.image));
            }
            const auto existing = FindProvider(current->entries_, provider);
            if (existing != current->entries_.end() && existing->Provider() == provider) {
                return std::unexpected(
                    Failure(BackendPluginCatalogErrorCode::DuplicateProvider, provider, admission.image));
            }
            if (const BackendPluginCatalogEntry* conflict = FindImage(current->entries_, admission.image)) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::ImageIdentityConflict, provider,
                                               admission.image, 0, 0, conflict->Provider(), conflict->Image()));
            }
            if (current->entries_.size() >= limits_.maximumPlugins ||
                current->entries_.size() >= current->entries_.max_size()) {
                return std::unexpected(
                    Failure(BackendPluginCatalogErrorCode::CapacityExceeded, provider, admission.image));
            }
            auto next = std::make_shared<BackendPluginCatalogSnapshot>(*current);
            const auto insertion = FindProvider(next->entries_, provider);
            prepared->revision_ = current->generation_ + 1;
            next->entries_.insert(insertion, std::move(*prepared));
            next->generation_ = current->generation_ + 1;
            const uint64_t generation = next->generation_;
            Publish(std::move(next));
            return generation;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::OutOfMemory, provider, admission.image));
        } catch (const std::length_error&) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::OutOfMemory, provider, admission.image));
        }
    }

    std::expected<uint64_t, BackendPluginCatalogError>
    BackendPluginCatalog::Replace(BackendPluginCatalogEntryVersion expected, PAL::ModulePtr module,
                                  BackendPluginAdmission admission) noexcept {
        if (!expected.Valid()) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::InvalidExpectedEntry, expected.provider,
                                           expected.image, expected.revision));
        }
        auto prepared = Prepare(std::move(module), admission);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        if (prepared->Provider() != expected.provider) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::ProviderMismatch, expected.provider,
                                           admission.image, expected.revision, 0, prepared->Provider()));
        }
        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            const auto found = FindProvider(current->entries_, expected.provider);
            if (found == current->entries_.end() || found->Provider() != expected.provider) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::NotFound, expected.provider,
                                               expected.image, expected.revision));
            }
            if (found->Version() != expected) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::StaleEntry, expected.provider,
                                               expected.image, expected.revision, found->Version().revision,
                                               found->Provider(), found->Image()));
            }
            if (current->generation_ == std::numeric_limits<uint64_t>::max()) {
                return std::unexpected(
                    Failure(BackendPluginCatalogErrorCode::GenerationExhausted, expected.provider, admission.image));
            }
            if (const BackendPluginCatalogEntry* conflict =
                    FindImage(current->entries_, admission.image, expected.provider)) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::ImageIdentityConflict, expected.provider,
                                               admission.image, expected.revision, 0, conflict->Provider(),
                                               conflict->Image()));
            }
            auto next = std::make_shared<BackendPluginCatalogSnapshot>(*current);
            const auto replacement = FindProvider(next->entries_, expected.provider);
            prepared->revision_ = current->generation_ + 1;
            *replacement = std::move(*prepared);
            next->generation_ = current->generation_ + 1;
            const uint64_t generation = next->generation_;
            Publish(std::move(next));
            return generation;
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                Failure(BackendPluginCatalogErrorCode::OutOfMemory, expected.provider, admission.image));
        }
    }

    std::expected<bool, BackendPluginCatalogError>
    BackendPluginCatalog::Retire(BackendPluginCatalogEntryVersion expected) noexcept {
        if (!expected.Valid()) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::InvalidExpectedEntry, expected.provider,
                                           expected.image, expected.revision));
        }
        try {
            std::lock_guard lock{writer_};
            const auto current = Snapshot();
            const auto found = FindProvider(current->entries_, expected.provider);
            if (found == current->entries_.end() || found->Provider() != expected.provider) {
                return false;
            }
            if (found->Version() != expected) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::StaleEntry, expected.provider,
                                               expected.image, expected.revision, found->Version().revision,
                                               found->Provider(), found->Image()));
            }
            if (current->generation_ == std::numeric_limits<uint64_t>::max()) {
                return std::unexpected(Failure(BackendPluginCatalogErrorCode::GenerationExhausted, expected.provider,
                                               expected.image, expected.revision));
            }
            auto next = std::make_shared<BackendPluginCatalogSnapshot>(*current);
            const auto retired = FindProvider(next->entries_, expected.provider);
            next->entries_.erase(retired);
            next->generation_ = current->generation_ + 1;
            Publish(std::move(next));
            return true;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(BackendPluginCatalogErrorCode::OutOfMemory, expected.provider,
                                           expected.image, expected.revision));
        }
    }

} // namespace Sora::Compute
