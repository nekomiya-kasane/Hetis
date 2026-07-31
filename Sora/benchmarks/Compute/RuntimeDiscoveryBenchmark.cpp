/**
 * @file RuntimeDiscoveryBenchmark.cpp
 * @brief Homogeneous backend-set projection baselines for target-machine rediscovery.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Runtime/BackendPlugin.h>
#include <Sora/Compute/Runtime/BackendPluginCatalog.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kBackendCount = 8;
    constexpr size_t kSampleCount = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::RuntimeProviderId kProvider{0x534F52415F435055ULL, 1};
    constexpr Sora::Compute::RuntimeProviderId kPluginProvider{0x44594E414D49435FULL, 0x504C5547494E5F31ULL};
    constexpr Sora::Compute::BackendPluginImageIdentity kPluginImage{{1, 2, 3, 4}};

    class RuntimeDiscoveryFixture final {
    public:
        [[nodiscard]] static std::expected<RuntimeDiscoveryFixture, Sora::Compute::CpuBackendError>
        Create(bool loadPlugin = true) noexcept {
            try {
                std::vector<Sora::Compute::CpuBackend> backends;
                backends.reserve(kBackendCount);
                for (size_t index = 0; index < kBackendCount; ++index) {
                    auto backend = Sora::Compute::CpuBackend::Create({.workerCount = 1});
                    if (!backend) {
                        return std::unexpected(backend.error());
                    }
                    backends.push_back(std::move(*backend));
                }
                RuntimeDiscoveryFixture fixture{std::move(backends)};
                if (loadPlugin && !fixture.LoadPlugin()) {
                    return std::unexpected(Sora::Compute::CpuBackendError{
                        Sora::Compute::CpuBackendErrorCode::ResourceConstructionFailed, 1});
                }
                return fixture;
            } catch (const std::bad_alloc&) {
                return std::unexpected(
                    Sora::Compute::CpuBackendError{Sora::Compute::CpuBackendErrorCode::OutOfMemory, kBackendCount});
            } catch (...) {
                return std::unexpected(Sora::Compute::CpuBackendError{
                    Sora::Compute::CpuBackendErrorCode::ResourceConstructionFailed, kBackendCount});
            }
        }

        void ProjectOne() noexcept { Project(1); }

        void ProjectEight() noexcept { Project(kBackendCount); }

        void ProjectLoadedPlugin() noexcept {
            if (!plugin_ && !LoadPlugin()) {
                failed_ = true;
                return;
            }
            auto contribution = (*plugin_)();
            if (!contribution) {
                failed_ = true;
                return;
            }
            checksum_ += contribution->devices.size() + contribution->resources.queues.size();
        }

        void LoadAndProjectPlugin() noexcept {
            constexpr std::string_view path = SORA_BACKEND_PLUGIN_FIXTURE_PATH;
            const Sora::PAL::ModuleLoadOptions options{.nameKind = Sora::PAL::ModuleNameKind::ExactPath,
                                                       .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
                                                       .bindMode = Sora::PAL::ModuleBindMode::Now,
                                                       .cachePolicy = Sora::PAL::ModuleCachePolicy::Private};
            auto module = Sora::PAL::LoadModule({path}, options);
            if (!module) {
                failed_ = true;
                return;
            }
            auto plugin = Sora::Compute::LoadRuntimeBackendPlugin(*module);
            if (!plugin) {
                failed_ = true;
                return;
            }
            auto contribution = (*plugin)();
            if (!contribution) {
                failed_ = true;
                return;
            }
            checksum_ += contribution->devices.size() + contribution->resources.queues.size();
        }

        void LoadAndRegisterPlugin() noexcept {
            Sora::Compute::BackendPluginCatalog catalog;
            auto module = LoadFixtureModule();
            if (!module) {
                failed_ = true;
                return;
            }
            auto registered =
                catalog.Register(std::move(*module), Sora::Compute::BackendPluginAdmission::Direct(kPluginImage));
            if (!registered) {
                failed_ = true;
                return;
            }
            checksum_ += *registered;
        }

        void LookupCatalogPlugin() noexcept {
            const auto snapshot = catalog_->Snapshot();
            const auto* plugin = snapshot->Find(kPluginProvider);
            if (plugin == nullptr) {
                failed_ = true;
                return;
            }
            checksum_ += plugin->Image().words[0];
        }

        void ProjectCatalogPlugin() noexcept {
            const auto snapshot = catalog_->Snapshot();
            const auto* plugin = snapshot->Find(kPluginProvider);
            if (plugin == nullptr) {
                failed_ = true;
                return;
            }
            auto contribution = plugin->Discover();
            if (!contribution) {
                failed_ = true;
                return;
            }
            checksum_ += contribution->devices.size() + contribution->resources.queues.size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        explicit RuntimeDiscoveryFixture(std::vector<Sora::Compute::CpuBackend> backends) noexcept
            : backends_(std::move(backends)) {}

        void Project(size_t count) noexcept {
            const auto selected = std::span<const Sora::Compute::CpuBackend>{backends_}.first(count);
            auto contribution = Sora::Compute::MakeRuntimeDiscoveryContribution(kProvider, selected);
            if (!contribution) {
                failed_ = true;
                return;
            }
            checksum_ += contribution->devices.size() + contribution->resources.transferEngines.size();
        }

        [[nodiscard]] static std::expected<Sora::PAL::ModulePtr, Sora::ErrorCode> LoadFixtureModule() noexcept {
            constexpr std::string_view path = SORA_BACKEND_PLUGIN_FIXTURE_PATH;
            const Sora::PAL::ModuleLoadOptions options{.nameKind = Sora::PAL::ModuleNameKind::ExactPath,
                                                       .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
                                                       .bindMode = Sora::PAL::ModuleBindMode::Now,
                                                       .cachePolicy = Sora::PAL::ModuleCachePolicy::Private};
            return Sora::PAL::LoadModule({path}, options);
        }

        [[nodiscard]] bool LoadPlugin() noexcept {
            constexpr std::string_view path = SORA_BACKEND_PLUGIN_FIXTURE_PATH;
            const Sora::PAL::ModuleLoadOptions options{.nameKind = Sora::PAL::ModuleNameKind::ExactPath,
                                                       .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
                                                       .bindMode = Sora::PAL::ModuleBindMode::Now,
                                                       .cachePolicy = Sora::PAL::ModuleCachePolicy::Private};
            auto module = Sora::PAL::LoadModule({path}, options);
            if (!module) {
                return false;
            }
            auto plugin = Sora::Compute::LoadRuntimeBackendPlugin(*module);
            if (!plugin) {
                return false;
            }
            plugin_.emplace(std::move(*plugin));
            try {
                catalog_ = std::make_unique<Sora::Compute::BackendPluginCatalog>();
            } catch (const std::bad_alloc&) {
                return false;
            }
            auto catalogModule = LoadFixtureModule();
            if (!catalogModule) {
                return false;
            }
            auto registered = catalog_->Register(std::move(*catalogModule),
                                                 Sora::Compute::BackendPluginAdmission::Direct(kPluginImage));
            if (!registered) {
                return false;
            }
            return true;
        }

        std::vector<Sora::Compute::CpuBackend> backends_{};
        std::optional<Sora::Compute::RuntimeBackendPlugin> plugin_{};
        std::unique_ptr<Sora::Compute::BackendPluginCatalog> catalog_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main(int argc, char** argv) {
    const bool coldPlugin = argc == 2 && std::string_view{argv[1]} == "--cold-plugin";
    const bool coldCatalog = argc == 2 && std::string_view{argv[1]} == "--cold-catalog";
    auto fixture = RuntimeDiscoveryFixture::Create(!coldPlugin && !coldCatalog);
    if (!fixture) {
        std::println(stderr, "CPU backend construction failed: code={}, detail={}",
                     std::to_underlying(fixture.error().code), fixture.error().detail);
        return 2;
    }
    if (coldPlugin) {
        const auto start = std::chrono::steady_clock::now();
        fixture->LoadAndProjectPlugin();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (fixture->Failed()) {
            return 2;
        }
        std::println("family,backends,metric,value,unit");
        std::println("runtime_plugin_discovery,1,cold_load_negotiate_project,{:.6f},ns/contribution",
                     std::chrono::duration<double, std::nano>{elapsed}.count());
        return 0;
    }
    if (coldCatalog) {
        const auto start = std::chrono::steady_clock::now();
        fixture->LoadAndRegisterPlugin();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (fixture->Failed()) {
            return 2;
        }
        std::println("family,backends,metric,value,unit");
        std::println("runtime_plugin_catalog,1,cold_load_negotiate_register,{:.6f},ns/publication",
                     std::chrono::duration<double, std::nano>{elapsed}.count());
        return 0;
    }

    auto one = [&fixture]() noexcept { fixture->ProjectOne(); };
    auto eight = [&fixture]() noexcept { fixture->ProjectEight(); };
    auto loadedPlugin = [&fixture]() noexcept { fixture->ProjectLoadedPlugin(); };
    auto catalogLookup = [&fixture]() noexcept { fixture->LookupCatalogPlugin(); };
    auto catalogPlugin = [&fixture]() noexcept { fixture->ProjectCatalogPlugin(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("project_1_backend", one),
        Sora::Benchmark::MakeCase("project_8_backends", eight),
        Sora::Benchmark::MakeCase("project_1_loaded_plugin_service", loadedPlugin),
        Sora::Benchmark::MakeCase("catalog_snapshot_lookup", catalogLookup),
        Sora::Benchmark::MakeCase("catalog_snapshot_project", catalogPlugin),
    };
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, 1, kMinimumSampleTime);
    if (fixture->Failed()) {
        return 2;
    }

    std::println("family,backends,metric,value,unit");
    std::println("runtime_discovery_projection,1,complete_provider,{:.6f},ns/contribution", timing.MedianTime(0));
    std::println("runtime_discovery_projection,8,complete_provider,{:.6f},ns/contribution", timing.MedianTime(1));
    std::println("runtime_discovery_ratio,8,eight_over_one,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("runtime_plugin_discovery,1,loaded_project,{:.6f},ns/contribution", timing.MedianTime(2));
    std::println("runtime_plugin_catalog,1,snapshot_lookup,{:.6f},ns/lookup", timing.MedianTime(3));
    std::println("runtime_plugin_catalog,1,snapshot_project,{:.6f},ns/contribution", timing.MedianTime(4));
    std::println("runtime_plugin_catalog_ratio,1,catalog_over_loaded,{:.6f},ratio", timing.PairedRatio(4, 2));
    std::println("checksum,8,resource_projection,{},value", fixture->Checksum());
}
