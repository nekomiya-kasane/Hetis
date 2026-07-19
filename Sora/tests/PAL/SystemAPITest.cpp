/**
 * @file SystemAPITest.cpp
 * @brief Verify immutable system API initialization and native symbol resolution contracts.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <filesystem>
#include <string>
#include <thread>

namespace PAL = Sora::PAL;

namespace {

    using MissingSystemFunction = void (*)();

    static_assert(Sora::HasFlag(PAL::SectionFlag::Read | PAL::SectionFlag::Write, PAL::SectionFlag::Read));

#if defined(PLATFORM_WINDOWS)
    struct ScopedTestDirectory {
        std::filesystem::path path;

        ~ScopedTestDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
#endif

} // namespace

TEST_CASE("System API tables have stable process-lifetime identities", "[Sora.PAL.SystemAPI]") {
    REQUIRE(&PAL::CurrentProcessModule() == &PAL::CurrentProcessModule());
    REQUIRE(&PAL::LoadNativeErrorSystemAPI() == &PAL::LoadNativeErrorSystemAPI());
    REQUIRE(&PAL::LoadEnvironmentSystemAPI() == &PAL::LoadEnvironmentSystemAPI());
    REQUIRE(&PAL::LoadModuleSystemAPI() == &PAL::LoadModuleSystemAPI());
    REQUIRE(&PAL::LoadProcessSystemAPI() == &PAL::LoadProcessSystemAPI());
    REQUIRE(&PAL::LoadThreadSystemAPI() == &PAL::LoadThreadSystemAPI());
    REQUIRE(&PAL::LoadGlobalMemorySystemAPI() == &PAL::LoadGlobalMemorySystemAPI());
    REQUIRE(&PAL::LoadClipboardSystemAPI() == &PAL::LoadClipboardSystemAPI());
    REQUIRE(&PAL::LoadCrashSystemAPI() == &PAL::LoadCrashSystemAPI());
    REQUIRE(&PAL::LoadStackTraceSystemAPI() == &PAL::LoadStackTraceSystemAPI());
}

TEST_CASE("ModuleLoader preserves UTF-8 spellings and native Unicode search roots", "[Sora.PAL.SystemAPI]") {
#if defined(PLATFORM_WINDOWS)
    using GetModuleFileNameWideFunction =
        PAL::WindowsSystem::DWord(__stdcall*)(void*, wchar_t*, PAL::WindowsSystem::DWord);
    using GetCurrentProcessIdFunction = PAL::WindowsSystem::DWord(__stdcall*)();
    using CopyFileWideFunction =
        PAL::WindowsSystem::Bool(__stdcall*)(const wchar_t*, const wchar_t*, PAL::WindowsSystem::Bool);

    constexpr PAL::ModuleLoadOptions kernelOptions{
        .nameKind = PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Private,
    };
    const Sora::Result<PAL::ModulePtr> kernel = PAL::LoadModule({"kernel32.dll"}, kernelOptions);
    REQUIRE(kernel.has_value());
    const auto getCurrentProcessId = (*kernel)->TryFindFunction<GetCurrentProcessIdFunction>("GetCurrentProcessId");
    REQUIRE(getCurrentProcessId != nullptr);
    const auto getModuleFileName =
        (*kernel)->TryFindFunction<GetModuleFileNameWideFunction>("GetModuleFileNameW");
    REQUIRE(getModuleFileName != nullptr);
    const auto copyFile = (*kernel)->TryFindFunction<CopyFileWideFunction>("CopyFileW");
    REQUIRE(copyFile != nullptr);

    std::array<wchar_t, 32768> executablePath{};
    const auto pathLength = getModuleFileName(nullptr, executablePath.data(), executablePath.size());
    REQUIRE(pathLength > 0);
    REQUIRE(pathLength < executablePath.size());

    const std::filesystem::path runtimeDirectory =
        std::filesystem::path{executablePath.data(), executablePath.data() + pathLength}.parent_path();
    const std::filesystem::path runtimeLibrary = runtimeDirectory / "c++.dll";
    REQUIRE(std::filesystem::is_regular_file(runtimeLibrary));

    ScopedTestDirectory temporary{
        .path = std::filesystem::temp_directory_path() /
                (L"Sora.Module-\u8def\u5f84-" + std::to_wstring(getCurrentProcessId())),
    };
    std::error_code error;
    std::filesystem::remove_all(temporary.path, error);
    REQUIRE(std::filesystem::create_directories(temporary.path));
    const std::filesystem::path fixture = temporary.path / "SoraUnicodeFixture.dll";
    REQUIRE(copyFile(runtimeLibrary.c_str(), fixture.c_str(), PAL::WindowsSystem::kFalse) !=
            PAL::WindowsSystem::kFalse);

    const std::array searchRoots{temporary.path};
    const PAL::ModuleLoadOptions searchOptions{
        .nameKind = PAL::ModuleNameKind::FileName,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Private,
        .searchPaths = searchRoots,
    };
    {
        const Sora::Result<PAL::ModulePtr> loaded = PAL::LoadModule({"SoraUnicodeFixture.dll"}, searchOptions);
        REQUIRE(loaded.has_value());
        REQUIRE((*loaded)->Path() == fixture);
    }

    const std::u8string fixtureUtf8 = fixture.generic_u8string();
    const std::string fixtureName{reinterpret_cast<const char*>(fixtureUtf8.data()), fixtureUtf8.size()};
    const PAL::ModuleLoadOptions exactOptions{
        .nameKind = PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Private,
    };
    const Sora::Result<PAL::ModulePtr> loaded = PAL::LoadModule({fixtureName}, exactOptions);
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->Path() == fixture);
#else
    SUCCEED("Native Unicode paths are covered by std::filesystem on this platform.");
#endif
}

TEST_CASE("ModuleLoader publishes one shared module during concurrent first load", "[Sora.PAL.SystemAPI]") {
#if defined(PLATFORM_WINDOWS)
    constexpr size_t kWorkerCount = 8;
    constexpr PAL::ModuleLoadOptions options{
        .nameKind = PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Shared,
    };
    std::barrier start{static_cast<ptrdiff_t>(kWorkerCount)};
    std::array<Sora::Result<PAL::ModulePtr>, kWorkerCount> results;
    std::array<std::jthread, kWorkerCount> workers;

    for (size_t index = 0; index < kWorkerCount; ++index) {
        workers[index] = std::jthread{[&, index] {
            start.arrive_and_wait();
            results[index] = PAL::LoadModule({"kernel32.dll"}, options);
        }};
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }

    for (const Sora::Result<PAL::ModulePtr>& result : results) {
        REQUIRE(result.has_value());
        REQUIRE(*result == *results.front());
    }
#else
    SUCCEED("Concurrent shared loading is covered on the primary Windows platform.");
#endif
}

TEST_CASE("System API resolves known exports and rejects missing exports", "[Sora.PAL.SystemAPI]") {
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    const PAL::ModulePtr& processModule = PAL::CurrentProcessModule();
    REQUIRE(processModule != nullptr);
    REQUIRE(processModule->Path().empty());
    REQUIRE_FALSE(processModule->UnloadOnDestroy());
#endif
#if defined(PLATFORM_WINDOWS)
    constexpr PAL::ModuleLoadOptions options{
        .nameKind = PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Private,
    };
    const Sora::Result<PAL::ModulePtr> loaded = PAL::LoadModule({"kernel32.dll"}, options);
    REQUIRE(loaded.has_value());
    const PAL::ModulePtr& module = *loaded;
    const auto getCurrentThreadId =
        module->TryFindFunction<PAL::ThreadSystemAPI::GetCurrentThreadIdFunction>("GetCurrentThreadId");
    REQUIRE(getCurrentThreadId != nullptr);
    REQUIRE(getCurrentThreadId() != 0);
    REQUIRE(module->TryFindFunction<MissingSystemFunction>("SoraSymbolThatMustNotExist") == nullptr);
    constexpr std::string_view decoratedName = "GetCurrentThreadId.trailing-data";
    const auto boundedName = decoratedName.substr(0, decoratedName.find('.'));
    REQUIRE(module->TryFindFunction<PAL::ThreadSystemAPI::GetCurrentThreadIdFunction>(boundedName) != nullptr);
    REQUIRE(module->TryFindFunction<MissingSystemFunction>(std::string_view{}) == nullptr);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    using AllocateFunction = void* (*)(size_t);
    REQUIRE(processModule->TryFindFunction<AllocateFunction>("malloc") != nullptr);
    REQUIRE(processModule->TryFindFunction<MissingSystemFunction>("SoraSymbolThatMustNotExist") == nullptr);
    REQUIRE(processModule->TryFindFunction<MissingSystemFunction>(std::string_view{}) == nullptr);
#else
    REQUIRE(PAL::CurrentProcessModule() == nullptr);
#endif
}

TEST_CASE("Required system API subsets are available on the running platform", "[Sora.PAL.SystemAPI]") {
#if defined(PLATFORM_WINDOWS)
    const PAL::NativeErrorSystemAPI& error = PAL::LoadNativeErrorSystemAPI();
    REQUIRE(error.getLastError != nullptr);
    REQUIRE(error.setLastError != nullptr);
    REQUIRE(error.formatMessageWide != nullptr);
    REQUIRE(error.localFree != nullptr);

    const PAL::EnvironmentSystemAPI& environment = PAL::LoadEnvironmentSystemAPI();
    REQUIRE(environment.getEnvironmentVariableWide != nullptr);
    REQUIRE(environment.setEnvironmentVariableWide != nullptr);
    REQUIRE(environment.getEnvironmentStringsWide != nullptr);
    REQUIRE(environment.freeEnvironmentStringsWide != nullptr);

    const PAL::ModuleSystemAPI& module = PAL::LoadModuleSystemAPI();
    REQUIRE(module.loadLibraryWide != nullptr);
    REQUIRE(module.getModuleHandleWide != nullptr);
    REQUIRE(module.findSymbol != nullptr);
    REQUIRE(module.freeLibrary != nullptr);

    const PAL::ProcessSystemAPI& process = PAL::LoadProcessSystemAPI();
    REQUIRE(process.getCurrentProcessId != nullptr);
    REQUIRE(process.getCurrentProcess != nullptr);
    REQUIRE(process.createProcessSnapshot != nullptr);
    REQUIRE(process.firstProcess != nullptr);
    REQUIRE(process.nextProcess != nullptr);
    REQUIRE(process.closeHandle != nullptr);
    REQUIRE(process.queryParentProcessId != nullptr);
    REQUIRE(process.queryFullProcessImageNameWide != nullptr);
    REQUIRE(process.getCommandLineWide != nullptr);
    REQUIRE(process.commandLineToArgvWide != nullptr);
    REQUIRE(process.localFree != nullptr);
    REQUIRE(process.getProcessTimes != nullptr);
    REQUIRE(process.getProcessMemoryInfo != nullptr);
    REQUIRE(process.captureUsage != nullptr);
    const PAL::ThreadSystemAPI& thread = PAL::LoadThreadSystemAPI();
    REQUIRE(thread.getCurrentThreadId != nullptr);
    REQUIRE(thread.getCurrentThread != nullptr);
    REQUIRE(thread.localFree != nullptr);
    REQUIRE(thread.getCurrentThreadId() != 0);

    const PAL::GlobalMemorySystemAPI& globalMemory = PAL::LoadGlobalMemorySystemAPI();
    REQUIRE(globalMemory.globalAllocate != nullptr);
    REQUIRE(globalMemory.globalFree != nullptr);
    REQUIRE(globalMemory.globalLock != nullptr);
    REQUIRE(globalMemory.globalUnlock != nullptr);
    REQUIRE(globalMemory.globalSize != nullptr);

    const PAL::ClipboardSystemAPI& clipboard = PAL::LoadClipboardSystemAPI();
    REQUIRE(clipboard.openClipboard != nullptr);
    REQUIRE(clipboard.closeClipboard != nullptr);
    REQUIRE(clipboard.getClipboardData != nullptr);
    REQUIRE(clipboard.setClipboardData != nullptr);

    const PAL::FileSystemAPI& file = PAL::LoadFileSystemAPI();
    REQUIRE(file.getStandardHandle != nullptr);
    REQUIRE(file.createFileWide != nullptr);
    REQUIRE(file.closeHandle != nullptr);
    REQUIRE(file.writeFile != nullptr);
    REQUIRE(file.flushFileBuffers != nullptr);

    const PAL::CrashSystemAPI& crash = PAL::LoadCrashSystemAPI();
    REQUIRE(crash.setUnhandledExceptionFilter != nullptr);

    PAL::LockedDbgHelpSystemAPI dbgHelp = PAL::LockDbgHelpSystemAPI();
    REQUIRE(dbgHelp->symSetOptions != nullptr);
    REQUIRE(dbgHelp->symInitialize != nullptr);
    REQUIRE(dbgHelp->symFromAddress != nullptr);
    REQUIRE(dbgHelp->symGetLineFromAddress != nullptr);
    REQUIRE(dbgHelp->symGetModuleInfo != nullptr);
    REQUIRE(dbgHelp->undecorateSymbolName != nullptr);

    const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
    REQUIRE(stackTrace.captureStackBackTrace != nullptr);
    REQUIRE(stackTrace.getCurrentProcess != nullptr);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    const PAL::EnvironmentSystemAPI& environment = PAL::LoadEnvironmentSystemAPI();
    REQUIRE(environment.get != nullptr);
    REQUIRE(environment.set != nullptr);
    REQUIRE(environment.remove != nullptr);

    const PAL::ModuleSystemAPI& module = PAL::LoadModuleSystemAPI();
    REQUIRE(module.open != nullptr);
    REQUIRE(module.close != nullptr);
    REQUIRE(module.findSymbol != nullptr);

    const PAL::ProcessSystemAPI& process = PAL::LoadProcessSystemAPI();
    REQUIRE(process.getProcessId != nullptr);
    REQUIRE(process.getParentProcessId != nullptr);
    REQUIRE(process.getResourceUsage != nullptr);
    REQUIRE(process.captureUsage != nullptr);
#    if defined(PLATFORM_MACOS)
    REQUIRE(process.getExecutablePath != nullptr);
    REQUIRE(process.getArgumentCount != nullptr);
    REQUIRE(process.getArgumentVector != nullptr);
    REQUIRE(process.processResourceUsage != nullptr);
#    endif

    const PAL::ThreadSystemAPI& thread = PAL::LoadThreadSystemAPI();
    REQUIRE(thread.pthreadSelf != nullptr);
    REQUIRE(thread.pthreadGetName != nullptr);

    const PAL::FileSystemAPI& file = PAL::LoadFileSystemAPI();
    REQUIRE(file.open != nullptr);
    REQUIRE(file.close != nullptr);
    REQUIRE(file.write != nullptr);
    REQUIRE(file.sync != nullptr);

    const PAL::CrashSystemAPI& crash = PAL::LoadCrashSystemAPI();
    REQUIRE(crash.signalAction != nullptr);
    REQUIRE(crash.emptySignalSet != nullptr);
    REQUIRE(crash.raiseSignal != nullptr);
    REQUIRE(crash.immediateExit != nullptr);

    const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
    REQUIRE(stackTrace.captureStackBackTrace != nullptr);
    REQUIRE(stackTrace.findDynamicSymbol != nullptr);
#else
    SUCCEED("The unsupported-platform tables intentionally contain no entry points.");
#endif
}
