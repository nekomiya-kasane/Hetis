/**
 * @file BackendPluginCatalogTest.cpp
 * @brief Admission, immutable-generation, and system-discovery tests for backend plugin catalogs.
 * @ingroup Testing
 */

#include <Sora/Compute/Runtime/BackendPluginCatalog.h>
#include <Sora/Compute/SystemComputeContext.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace {

    using namespace Sora::Compute;

    constexpr RuntimeProviderId kFixtureProvider{0x44594E414D49435FULL, 0x504C5547494E5F31ULL};
    constexpr BackendPluginImageIdentity kImageOne{{1, 2, 3, 4}};
    constexpr BackendPluginImageIdentity kImageTwo{{5, 6, 7, 8}};
    constexpr SemanticHash kAuthority{0x415554484F524954ULL, 0x595F303030303031ULL};

    [[nodiscard]] std::expected<Sora::PAL::ModulePtr, Sora::ErrorCode> LoadFixture() {
        constexpr std::string_view path = SORA_BACKEND_PLUGIN_FIXTURE_PATH;
        const Sora::PAL::ModuleLoadOptions options{.nameKind = Sora::PAL::ModuleNameKind::ExactPath,
                                                   .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
                                                   .bindMode = Sora::PAL::ModuleBindMode::Now,
                                                   .cachePolicy = Sora::PAL::ModuleCachePolicy::Private};
        return Sora::PAL::LoadModule({path}, options);
    }

    [[nodiscard]] SystemRuntimeConfig CpuOnlyRuntime(std::shared_ptr<const BackendPluginCatalog> plugins = {}) {
        return {.cpu = {.workerCount = 1},
                .cpuCode = SystemCpuCodePolicy::InterpreterOnly,
                .vulkanPolicy = SystemVulkanPolicy::Disabled,
                .backendPlugins = std::move(plugins)};
    }

} // namespace

TEST_CASE("Backend plugin admission keeps image trust orthogonal to provider and path identity",
          "[Sora][Compute][Runtime][Plugin][Admission]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<BackendPluginImageIdentity>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<BackendPluginAdmission>);

    const BackendPluginAdmission direct = BackendPluginAdmission::Direct(kImageOne, 7);
    REQUIRE(direct.Valid());
    REQUIRE(direct.kind == BackendPluginAdmissionKind::Direct);
    REQUIRE(direct.authority == kDirectBackendPluginAuthority);
    REQUIRE(direct.image == kImageOne);

    const BackendPluginAdmission verified = BackendPluginAdmission::Verified(9, kAuthority, kImageTwo, 11);
    REQUIRE(verified.Valid());
    REQUIRE(verified.kind == BackendPluginAdmissionKind::VerifiedImage);
    REQUIRE(verified.scheme == 9);
    REQUIRE(verified.authority == kAuthority);

    REQUIRE_FALSE(BackendPluginAdmission::Direct({}).Valid());
    REQUIRE_FALSE(BackendPluginAdmission::Verified(0, kAuthority, kImageOne, 1).Valid());
    REQUIRE_FALSE(BackendPluginAdmission::Verified(1, {}, kImageOne, 1).Valid());
}

TEST_CASE("Backend plugin catalog publishes one immutable canonical provider generation",
          "[Sora][Compute][Runtime][Plugin][Catalog]") {
    BackendPluginCatalog catalog;
    const auto empty = catalog.Snapshot();
    REQUIRE(empty->Generation() == 0);
    REQUIRE(empty->Entries().empty());

    auto module = LoadFixture();
    REQUIRE(module.has_value());
    auto registered = catalog.Register(std::move(*module), BackendPluginAdmission::Direct(kImageOne));
    REQUIRE(registered.has_value());
    REQUIRE(*registered == 1);

    const auto current = catalog.Snapshot();
    REQUIRE(current->Generation() == 1);
    REQUIRE(current->Entries().size() == 1);
    REQUIRE(current->Find(kFixtureProvider) != nullptr);
    REQUIRE(current->Find(kFixtureProvider)->Provider() == kFixtureProvider);
    REQUIRE(current->Find(kFixtureProvider)->Image() == kImageOne);
    REQUIRE(current->Find(kFixtureProvider)->Version().revision == 1);
    REQUIRE_FALSE(current->Find(kFixtureProvider)->Path().empty());
    REQUIRE(empty->Entries().empty());

    auto contribution = current->Find(kFixtureProvider)->Discover();
    REQUIRE(contribution.has_value());
    REQUIRE(contribution->provider == kFixtureProvider);
    REQUIRE(contribution->devices.size() == 1);
}

TEST_CASE("Backend plugin catalog register replace and retire use exact entry-version transactions",
          "[Sora][Compute][Runtime][Plugin][Catalog][Transaction]") {
    std::shared_ptr<const BackendPluginCatalogSnapshot> original;
    std::shared_ptr<const BackendPluginCatalogSnapshot> replacement;
    {
        BackendPluginCatalog catalog;
        auto module = LoadFixture();
        REQUIRE(module.has_value());
        auto registered = catalog.Register(std::move(*module), BackendPluginAdmission::Direct(kImageOne));
        REQUIRE(registered.has_value());
        REQUIRE(*registered == 1);
        original = catalog.Snapshot();
        const BackendPluginCatalogEntryVersion originalVersion = original->Find(kFixtureProvider)->Version();

        auto duplicateModule = LoadFixture();
        REQUIRE(duplicateModule.has_value());
        auto duplicate = catalog.Register(std::move(*duplicateModule), BackendPluginAdmission::Direct(kImageOne));
        REQUIRE_FALSE(duplicate.has_value());
        REQUIRE(duplicate.error().code == BackendPluginCatalogErrorCode::DuplicateProvider);
        REQUIRE(catalog.Snapshot() == original);

        auto staleModule = LoadFixture();
        REQUIRE(staleModule.has_value());
        auto stale = catalog.Replace({.provider = kFixtureProvider, .image = kImageTwo, .revision = 1},
                                     std::move(*staleModule), BackendPluginAdmission::Direct(kImageTwo));
        REQUIRE_FALSE(stale.has_value());
        REQUIRE(stale.error().code == BackendPluginCatalogErrorCode::StaleEntry);
        REQUIRE(catalog.Snapshot() == original);

        auto replacementModule = LoadFixture();
        REQUIRE(replacementModule.has_value());
        auto replaced = catalog.Replace(originalVersion, std::move(*replacementModule),
                                        BackendPluginAdmission::Verified(9, kAuthority, kImageOne, 12));
        REQUIRE(replaced.has_value());
        REQUIRE(*replaced == 2);
        replacement = catalog.Snapshot();
        REQUIRE(replacement != original);
        REQUIRE(replacement->Find(kFixtureProvider)->Image() == kImageOne);
        const BackendPluginCatalogEntryVersion replacementVersion = replacement->Find(kFixtureProvider)->Version();
        REQUIRE(replacementVersion.revision == 2);

        auto staleRetire = catalog.Retire(originalVersion);
        REQUIRE_FALSE(staleRetire.has_value());
        REQUIRE(staleRetire.error().code == BackendPluginCatalogErrorCode::StaleEntry);
        REQUIRE(staleRetire.error().actualImage == kImageOne);
        REQUIRE(staleRetire.error().expectedRevision == 1);
        REQUIRE(staleRetire.error().actualRevision == 2);

        auto retired = catalog.Retire(replacementVersion);
        REQUIRE(retired.has_value());
        REQUIRE(*retired);
        REQUIRE(catalog.Snapshot()->Generation() == 3);
        REQUIRE(catalog.Snapshot()->Entries().empty());
    }

    REQUIRE(original->Find(kFixtureProvider) != nullptr);
    REQUIRE(replacement->Find(kFixtureProvider) != nullptr);
    REQUIRE(original->Find(kFixtureProvider)->Discover().has_value());
    REQUIRE(replacement->Find(kFixtureProvider)->Discover().has_value());
}

TEST_CASE("System runtime acquires one current plugin catalog generation for every complete discovery",
          "[Sora][Compute][System][Plugin][Discovery]") {
    auto plugins = std::make_shared<BackendPluginCatalog>();
    auto factory = SystemRuntimeFactory::Create(CpuOnlyRuntime(plugins));
    REQUIRE(factory.has_value());

    auto builtInOnly = factory->Discover(1);
    REQUIRE(builtInOnly.has_value());
    REQUIRE(builtInOnly->Devices().size() == 1);

    auto coordinator = RuntimeRecoveryCoordinator::Create(std::move(*builtInOnly));
    REQUIRE(coordinator.has_value());
    const auto observed = (*coordinator)->Current();
    const auto failed = std::ranges::find_if(observed->Devices(), [](const RuntimeDeviceBinding& device) {
        return device.descriptor.backend == BackendKind::Cpu;
    });
    REQUIRE(failed != observed->Devices().end());
    const BackendServiceId failedService = failed->service;
    const RuntimeEpochError failure{.code = RuntimeEpochErrorCode::DeviceLost,
                                    .device = static_cast<uint32_t>(failed - observed->Devices().begin()),
                                    .cause = DeviceObservationError::Lost};

    auto module = LoadFixture();
    REQUIRE(module.has_value());
    auto registered = plugins->Register(std::move(*module), BackendPluginAdmission::Direct(kImageOne));
    REQUIRE(registered.has_value());
    REQUIRE(*registered == 1);
    auto withPlugin = factory->Discover(2);
    REQUIRE(withPlugin.has_value());
    REQUIRE(withPlugin->Devices().size() == 2);
    REQUIRE(std::ranges::any_of(withPlugin->Devices(), [](const RuntimeDeviceBinding& device) {
        return device.descriptor.id == DeviceId{0x44594E414D4943ULL, 1};
    }));

    auto recovered = (*coordinator)->Recover(observed, failure, *factory);
    REQUIRE(recovered.has_value());
    REQUIRE((*recovered)->Sequence() == 2);
    REQUIRE((*recovered)->Devices().size() == 2);
    REQUIRE(std::ranges::none_of((*recovered)->Devices(), [failedService](const RuntimeDeviceBinding& device) {
        return device.service == failedService;
    }));

    const BackendPluginCatalogEntryVersion pluginVersion = plugins->Snapshot()->Find(kFixtureProvider)->Version();
    auto retired = plugins->Retire(pluginVersion);
    REQUIRE(retired.has_value());
    REQUIRE(*retired);
    auto afterRetire = factory->Discover(3);
    REQUIRE(afterRetire.has_value());
    REQUIRE(afterRetire->Devices().size() == 1);
    REQUIRE(afterRetire->Devices().front().descriptor.backend == BackendKind::Cpu);
}
