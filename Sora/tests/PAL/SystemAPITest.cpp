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
    REQUIRE(&PAL::LoadLocalMemorySystemAPI() == &PAL::LoadLocalMemorySystemAPI());
    REQUIRE(&PAL::LoadHandleSystemAPI() == &PAL::LoadHandleSystemAPI());
    REQUIRE(&PAL::LoadSynchronizationSystemAPI() == &PAL::LoadSynchronizationSystemAPI());
    REQUIRE(&PAL::LoadVirtualMemorySystemAPI() == &PAL::LoadVirtualMemorySystemAPI());
    REQUIRE(&PAL::LoadFileMappingSystemAPI() == &PAL::LoadFileMappingSystemAPI());
    REQUIRE(&PAL::LoadSystemInformationSystemAPI() == &PAL::LoadSystemInformationSystemAPI());
    REQUIRE(&PAL::LoadEnvironmentSystemAPI() == &PAL::LoadEnvironmentSystemAPI());
    REQUIRE(&PAL::LoadModuleSystemAPI() == &PAL::LoadModuleSystemAPI());
    REQUIRE(&PAL::LoadProcessSystemAPI() == &PAL::LoadProcessSystemAPI());
    REQUIRE(&PAL::LoadProcessControlSystemAPI() == &PAL::LoadProcessControlSystemAPI());
    REQUIRE(&PAL::LoadThreadSystemAPI() == &PAL::LoadThreadSystemAPI());
    REQUIRE(&PAL::LoadThreadControlSystemAPI() == &PAL::LoadThreadControlSystemAPI());
    REQUIRE(&PAL::LoadRandomSystemAPI() == &PAL::LoadRandomSystemAPI());
    REQUIRE(&PAL::LoadSchedulerTraceSystemAPI() == &PAL::LoadSchedulerTraceSystemAPI());
    REQUIRE(&PAL::LoadGlobalMemorySystemAPI() == &PAL::LoadGlobalMemorySystemAPI());
    REQUIRE(&PAL::LoadClipboardSystemAPI() == &PAL::LoadClipboardSystemAPI());
    REQUIRE(&PAL::LoadDisplaySystemAPI() == &PAL::LoadDisplaySystemAPI());
    REQUIRE(&PAL::LoadWin32WindowSystemAPI() == &PAL::LoadWin32WindowSystemAPI());
    REQUIRE(&PAL::LoadX11WindowSystemAPI() == &PAL::LoadX11WindowSystemAPI());
    REQUIRE(&PAL::LoadXcbWindowSystemAPI() == &PAL::LoadXcbWindowSystemAPI());
    REQUIRE(&PAL::LoadWaylandWindowSystemAPI() == &PAL::LoadWaylandWindowSystemAPI());
    REQUIRE(&PAL::LoadStackTraceSystemAPI() == &PAL::LoadStackTraceSystemAPI());
    REQUIRE(&PAL::LoadSocketSystemAPI() == &PAL::LoadSocketSystemAPI());
    REQUIRE(&PAL::LoadTcpSystemAPI() == &PAL::LoadTcpSystemAPI());
    REQUIRE(&PAL::LoadUdpSystemAPI() == &PAL::LoadUdpSystemAPI());
    REQUIRE(&PAL::LoadClockSystemAPI() == &PAL::LoadClockSystemAPI());
    REQUIRE(&PAL::LoadAdvancedSocketSystemAPI() == &PAL::LoadAdvancedSocketSystemAPI());
    REQUIRE(&PAL::LoadNetworkInterfaceSystemAPI() == &PAL::LoadNetworkInterfaceSystemAPI());
    REQUIRE(&PAL::LoadModernFileSystemAPI() == &PAL::LoadModernFileSystemAPI());
    REQUIRE(&PAL::LoadFanotifySystemAPI() == &PAL::LoadFanotifySystemAPI());
    REQUIRE(&PAL::LoadLinuxSyscallSystemAPI() == &PAL::LoadLinuxSyscallSystemAPI());
    REQUIRE(&PAL::LoadPerfEventSystemAPI() == &PAL::LoadPerfEventSystemAPI());
#if defined(PLATFORM_WINDOWS)
    REQUIRE(&PAL::LoadWinsockOverlappedSystemAPI() == &PAL::LoadWinsockOverlappedSystemAPI());
    REQUIRE(&PAL::LoadWindowsIORingSystemAPI() == &PAL::LoadWindowsIORingSystemAPI());
#elif defined(PLATFORM_LINUX)
    REQUIRE(&PAL::LoadEpollSystemAPI() == &PAL::LoadEpollSystemAPI());
    REQUIRE(&PAL::LoadEventFdSystemAPI() == &PAL::LoadEventFdSystemAPI());
    REQUIRE(&PAL::LoadTimerFdSystemAPI() == &PAL::LoadTimerFdSystemAPI());
    REQUIRE(&PAL::LoadSignalFdSystemAPI() == &PAL::LoadSignalFdSystemAPI());
    REQUIRE(&PAL::LoadLinuxIoUringSystemAPI() == &PAL::LoadLinuxIoUringSystemAPI());
#endif
}

TEST_CASE("Socket API tables expose the complete transport split", "[Sora.PAL.SystemAPI]") {
    const PAL::SocketSystemAPI& socket = PAL::LoadSocketSystemAPI();
    const PAL::TcpSystemAPI& tcp = PAL::LoadTcpSystemAPI();
    const PAL::UdpSystemAPI& udp = PAL::LoadUdpSystemAPI();
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    REQUIRE(socket.create != nullptr);
    REQUIRE(socket.close != nullptr);
    REQUIRE(socket.bind != nullptr);
    REQUIRE(socket.getSocketName != nullptr);
    REQUIRE(socket.getPeerName != nullptr);
    REQUIRE(socket.setOption != nullptr);
    REQUIRE(socket.getOption != nullptr);
    REQUIRE(socket.ioctl != nullptr);
    REQUIRE(socket.poll != nullptr);
    REQUIRE(socket.getAddressInfo != nullptr);
    REQUIRE(socket.freeAddressInfo != nullptr);
    REQUIRE(socket.getNameInfo != nullptr);
    REQUIRE(tcp.listen != nullptr);
    REQUIRE(tcp.accept != nullptr);
    REQUIRE(tcp.connect != nullptr);
    REQUIRE(tcp.send != nullptr);
    REQUIRE(tcp.receive != nullptr);
    REQUIRE(tcp.shutdown != nullptr);
    REQUIRE(udp.sendTo != nullptr);
    REQUIRE(udp.receiveFrom != nullptr);
#    if defined(PLATFORM_WINDOWS)
    REQUIRE(socket.startup != nullptr);
    REQUIRE(socket.cleanup != nullptr);
    REQUIRE(socket.getLastError != nullptr);
    REQUIRE(socket.setLastError != nullptr);
#    endif
#else
    SUCCEED("Unsupported platforms publish empty socket tables.");
#endif
    REQUIRE(static_cast<int>(PAL::SocketSystemAPI::Type::Stream) !=
            static_cast<int>(PAL::SocketSystemAPI::Type::Datagram));
    REQUIRE(static_cast<int>(PAL::SocketSystemAPI::Protocol::Tcp) == 6);
    REQUIRE(static_cast<int>(PAL::SocketSystemAPI::Protocol::Udp) == 17);
}

TEST_CASE("Asynchronous I/O tables expose platform-native completion and readiness entries", "[Sora.PAL.SystemAPI]") {
    const PAL::ClockSystemAPI& clock = PAL::LoadClockSystemAPI();
#if defined(PLATFORM_WINDOWS)
    REQUIRE(clock.queryCounter != nullptr);
    REQUIRE(clock.queryFrequency != nullptr);
    REQUIRE(clock.getPreciseSystemTime != nullptr);

    const PAL::WinsockOverlappedSystemAPI& winsock = PAL::LoadWinsockOverlappedSystemAPI();
    REQUIRE(winsock.create != nullptr);
    REQUIRE(winsock.send != nullptr);
    REQUIRE(winsock.receive != nullptr);
    REQUIRE(winsock.sendTo != nullptr);
    REQUIRE(winsock.receiveFrom != nullptr);
    REQUIRE(winsock.getResult != nullptr);
    REQUIRE(winsock.control != nullptr);

    const PAL::IOCPSystemAPI& iocp = PAL::LoadIOCPSystemAPI();
    REQUIRE(iocp.getQueuedCompletionStatusEx != nullptr);
    REQUIRE(iocp.setFileCompletionNotificationModes != nullptr);
    const PAL::WindowsIORingSystemAPI& ioRing = PAL::LoadWindowsIORingSystemAPI();
    if (ioRing.createIoRing != nullptr) {
        REQUIRE(ioRing.queryCapabilities != nullptr);
        REQUIRE(ioRing.getIoRingInfo != nullptr);
        REQUIRE(ioRing.submitIoRing != nullptr);
        REQUIRE(ioRing.closeIoRing != nullptr);
        REQUIRE(ioRing.popIoRingCompletion != nullptr);
    }
#elif defined(PLATFORM_LINUX)
    REQUIRE(clock.getTime != nullptr);
    REQUIRE(clock.getResolution != nullptr);
    REQUIRE(clock.sleep != nullptr);
    REQUIRE(PAL::LoadEpollSystemAPI().create != nullptr);
    REQUIRE(PAL::LoadEpollSystemAPI().control != nullptr);
    REQUIRE(PAL::LoadEpollSystemAPI().wait != nullptr);
    REQUIRE(PAL::LoadEventFdSystemAPI().create != nullptr);
    REQUIRE(PAL::LoadTimerFdSystemAPI().create != nullptr);
    REQUIRE(PAL::LoadTimerFdSystemAPI().setTime != nullptr);
    REQUIRE(PAL::LoadTimerFdSystemAPI().getTime != nullptr);
    REQUIRE(PAL::LoadSignalFdSystemAPI().create != nullptr);
    const PAL::LinuxIoUringSystemAPI& ioRing = PAL::LoadLinuxIoUringSystemAPI();
    if (ioRing.queueInit != nullptr) {
        REQUIRE(ioRing.queueExit != nullptr);
        REQUIRE(ioRing.submit != nullptr);
        REQUIRE(ioRing.peekBatchCompletion != nullptr);
        REQUIRE(ioRing.waitCompletions != nullptr);
    }
#elif defined(PLATFORM_MACOS)
    REQUIRE(clock.getTime != nullptr);
    REQUIRE(clock.getResolution != nullptr);
#else
    SUCCEED("Unsupported platforms publish empty asynchronous I/O tables.");
#endif
}

TEST_CASE("P1 and P2 tables preserve platform-specific capability boundaries", "[Sora.PAL.SystemAPI]") {
#if defined(PLATFORM_WINDOWS)
    const auto& synchronization = PAL::LoadSynchronizationSystemAPI();
    REQUIRE(synchronization.waitForSingleObject != nullptr);
    REQUIRE(synchronization.waitForMultipleObjects != nullptr);
    REQUIRE(synchronization.createMutexExWide != nullptr);
    REQUIRE(synchronization.releaseMutex != nullptr);

    const auto& memory = PAL::LoadVirtualMemorySystemAPI();
    REQUIRE(memory.allocate != nullptr);
    REQUIRE(memory.free != nullptr);
    REQUIRE(memory.protect != nullptr);
    REQUIRE(memory.query != nullptr);

    const auto& process = PAL::LoadProcessControlSystemAPI();
    REQUIRE(process.openProcess != nullptr);
    REQUIRE(process.getExitCode != nullptr);
    REQUIRE(process.terminate != nullptr);
    REQUIRE(process.setPriorityClass != nullptr);

    const auto& thread = PAL::LoadThreadControlSystemAPI();
    REQUIRE(thread.setPriority != nullptr);
    REQUIRE(thread.getPriority != nullptr);
    REQUIRE(thread.setAffinityMask != nullptr);
    REQUIRE(thread.suspend != nullptr);
    REQUIRE(thread.resume != nullptr);

    REQUIRE(PAL::LoadRandomSystemAPI().generate != nullptr);
    REQUIRE(PAL::LoadModernFileSystemAPI().createDirectoryWide != nullptr);
#elif defined(PLATFORM_LINUX)
    const auto& memory = PAL::LoadVirtualMemorySystemAPI();
    REQUIRE(memory.protect != nullptr);
    REQUIRE(memory.lock != nullptr);
    REQUIRE(memory.unlock != nullptr);

    const auto& socket = PAL::LoadAdvancedSocketSystemAPI();
    REQUIRE(socket.sendMessage != nullptr);
    REQUIRE(socket.receiveMessage != nullptr);

    const auto& network = PAL::LoadNetworkInterfaceSystemAPI();
    REQUIRE(network.getIfAddresses != nullptr);
    REQUIRE(network.freeIfAddresses != nullptr);
    REQUIRE(network.ifNameToIndex != nullptr);

    const auto& process = PAL::LoadProcessControlSystemAPI();
    REQUIRE(process.fork != nullptr);
    REQUIRE(process.exec != nullptr);
    REQUIRE(process.wait != nullptr);
    REQUIRE(process.kill != nullptr);
    REQUIRE(process.signalAction != nullptr);

    const auto& files = PAL::LoadModernFileSystemAPI();
    REQUIRE(files.openAt != nullptr);
    REQUIRE(files.makeDirectoryAt != nullptr);
    REQUIRE(files.unlinkAt != nullptr);
    REQUIRE(files.renameAt != nullptr);
    REQUIRE(files.readLinkAt != nullptr);

    REQUIRE(PAL::LoadLinuxSyscallSystemAPI().syscall != nullptr);
#else
    SUCCEED("The P1/P2 tables are intentionally empty on unsupported platforms.");
#endif
}

TEST_CASE("Native window views enforce concrete backend identity", "[Sora.PAL.SystemAPI]") {
    int nativeToken = 0;
    const PAL::NativeWindowView win32{
        .windowSystem = Sora::Platform::WindowSystem::Win32,
        .backend = PAL::NativeWindowBackend::Win32,
        .window = reinterpret_cast<uintptr_t>(&nativeToken),
    };
    const PAL::NativeWindowView xlib{
        .windowSystem = Sora::Platform::WindowSystem::X11,
        .backend = PAL::NativeWindowBackend::Xlib,
        .displayConnection = &nativeToken,
        .window = 1,
    };
    const PAL::NativeWindowView xcb{
        .windowSystem = Sora::Platform::WindowSystem::X11,
        .backend = PAL::NativeWindowBackend::Xcb,
        .displayConnection = &nativeToken,
        .window = 1,
    };
    const PAL::NativeWindowView wayland{
        .windowSystem = Sora::Platform::WindowSystem::Wayland,
        .backend = PAL::NativeWindowBackend::Wayland,
        .displayConnection = &nativeToken,
        .surface = &nativeToken,
    };
    const PAL::NativeWindowView mismatched{
        .windowSystem = Sora::Platform::WindowSystem::X11,
        .backend = PAL::NativeWindowBackend::Wayland,
        .displayConnection = &nativeToken,
        .surface = &nativeToken,
    };

    REQUIRE(win32);
    REQUIRE(xlib);
    REQUIRE(xcb);
    REQUIRE(wayland);
    REQUIRE_FALSE(mismatched);
}

TEST_CASE("Window capabilities require the complete requested feature mask", "[Sora.PAL.SystemAPI]") {
    constexpr PAL::WindowSystemCapabilities capabilities{
        .features = std::to_underlying(PAL::WindowSystemFeature::WindowCreation) |
                    std::to_underlying(PAL::WindowSystemFeature::EventQueue),
    };
    constexpr auto complete =
        static_cast<PAL::WindowSystemFeature>(std::to_underlying(PAL::WindowSystemFeature::WindowCreation) |
                                              std::to_underlying(PAL::WindowSystemFeature::EventQueue));
    constexpr auto partial =
        static_cast<PAL::WindowSystemFeature>(std::to_underlying(PAL::WindowSystemFeature::WindowCreation) |
                                              std::to_underlying(PAL::WindowSystemFeature::WindowResize));

    STATIC_REQUIRE(capabilities.Supports(complete));
    STATIC_REQUIRE_FALSE(capabilities.Supports(partial));
    STATIC_REQUIRE_FALSE(capabilities.Supports(PAL::WindowSystemFeature::None));
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
    const auto getModuleFileName = (*kernel)->TryFindFunction<GetModuleFileNameWideFunction>("GetModuleFileNameW");
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

TEST_CASE("ModuleLoader rejects invalid UTF-8 before native path construction", "[Sora.PAL.SystemAPI]") {
    constexpr PAL::ModuleLoadOptions options{
        .nameKind = PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = PAL::ModuleCachePolicy::Private,
    };
    const std::string invalidName{static_cast<char>(0xC0)};
    const Sora::Result<PAL::ModulePtr> loaded = PAL::LoadModule({invalidName}, options);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error() == Sora::ErrorCode::OverlongUtf8Sequence);
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
    REQUIRE(PAL::LoadLocalMemorySystemAPI().localFree != nullptr);
    const PAL::HandleSystemAPI& handles = PAL::LoadHandleSystemAPI();
    REQUIRE(handles.closeHandle != nullptr);
    REQUIRE(handles.duplicateHandle != nullptr);
    const PAL::EventSystemAPI& events = PAL::LoadEventSystemAPI();
    REQUIRE(events.createEventWide != nullptr);
    REQUIRE(events.resetEvent != nullptr);
    const PAL::SynchronizationSystemAPI& synchronization = PAL::LoadSynchronizationSystemAPI();
    REQUIRE(synchronization.waitForSingleObject != nullptr);
    const PAL::FileMappingSystemAPI& mapping = PAL::LoadFileMappingSystemAPI();
    REQUIRE(mapping.createFileMappingWide != nullptr);
    REQUIRE(mapping.mapViewOfFile != nullptr);
    REQUIRE(mapping.unmapViewOfFile != nullptr);
    REQUIRE(mapping.flushViewOfFile != nullptr);
    REQUIRE(PAL::LoadSystemInformationSystemAPI().getSystemInfo != nullptr);

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
    REQUIRE(process.queryParentProcessId != nullptr);
    REQUIRE(process.queryFullProcessImageNameWide != nullptr);
    REQUIRE(process.getCommandLineWide != nullptr);
    REQUIRE(process.commandLineToArgvWide != nullptr);
    REQUIRE(process.getProcessTimes != nullptr);
    REQUIRE(process.getProcessMemoryInfo != nullptr);
    REQUIRE(process.captureUsage != nullptr);
    const PAL::ThreadSystemAPI& thread = PAL::LoadThreadSystemAPI();
    REQUIRE(thread.getCurrentThreadId != nullptr);
    REQUIRE(thread.getCurrentThread != nullptr);
    REQUIRE(thread.openThread != nullptr);
    REQUIRE(thread.getCurrentThreadId() != 0);

    const PAL::SchedulerTraceSystemAPI& tracing = PAL::LoadSchedulerTraceSystemAPI();
#    if defined(PLATFORM_WINDOWS)
    REQUIRE(tracing.startTrace != nullptr);
    REQUIRE(tracing.controlTrace != nullptr);
    REQUIRE(tracing.openTrace != nullptr);
    REQUIRE(tracing.processTrace != nullptr);
    REQUIRE(tracing.closeTrace != nullptr);
    REQUIRE(tracing.queryPerformanceFrequency != nullptr);
#    endif
    REQUIRE(tracing.start != nullptr);
    REQUIRE(tracing.process != nullptr);
    REQUIRE(tracing.stop != nullptr);
    REQUIRE(tracing.close != nullptr);

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

    const PAL::DisplaySystemAPI& display = PAL::LoadDisplaySystemAPI();
    REQUIRE(display.enumDisplayMonitors != nullptr);
    REQUIRE(display.getMonitorInfoWide != nullptr);
    REQUIRE(display.monitorFromPoint != nullptr);
    REQUIRE(display.monitorFromWindow != nullptr);
    REQUIRE(display.getWindowRect != nullptr);
    REQUIRE(display.getClientRect != nullptr);
    REQUIRE(display.clientToScreen != nullptr);
    REQUIRE(display.screenToClient != nullptr);
    REQUIRE(display.getCursorPosition != nullptr);
    REQUIRE(display.getSystemMetrics != nullptr);

    const PAL::Win32WindowSystemAPI& window = PAL::LoadWin32WindowSystemAPI();
    REQUIRE(window.Available());
    REQUIRE(window.registerClassExWide != nullptr);
    REQUIRE(window.createWindowExWide != nullptr);
    REQUIRE(window.destroyWindow != nullptr);
    REQUIRE(window.defWindowProc != nullptr);
    REQUIRE(window.peekMessage != nullptr);
    REQUIRE(window.translateMessage != nullptr);
    REQUIRE(window.dispatchMessage != nullptr);
    REQUIRE(window.DeclaredCapabilities().windowSystem == Sora::Platform::WindowSystem::Win32);
    REQUIRE(window.DeclaredCapabilities().Supports(PAL::WindowSystemFeature::VulkanSurface));

    const PAL::FileSystemAPI& file = PAL::LoadFileSystemAPI();
    REQUIRE(file.getStandardHandle != nullptr);
    REQUIRE(file.createFileWide != nullptr);
    REQUIRE(file.writeFile != nullptr);
    REQUIRE(file.flushFileBuffers != nullptr);

    PAL::LockedDbgHelpSystemAPI dbgHelp = PAL::LockDbgHelpSystemAPI();
    REQUIRE(dbgHelp->symSetOptions != nullptr);
    REQUIRE(dbgHelp->symInitialize != nullptr);
    REQUIRE(dbgHelp->symFromAddress != nullptr);
    REQUIRE(dbgHelp->symGetLineFromAddress != nullptr);
    REQUIRE(dbgHelp->symGetModuleInfo != nullptr);
    REQUIRE(dbgHelp->undecorateSymbolName != nullptr);
    PAL::DbgHelpAddressInfo unresolved;
    REQUIRE_FALSE(dbgHelp.ResolveAddress(nullptr, 0, unresolved));

    const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
    REQUIRE(stackTrace.captureStackBackTrace != nullptr);
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    const PAL::X11WindowSystemAPI& x11 = PAL::LoadX11WindowSystemAPI();
    const PAL::XcbWindowSystemAPI& xcb = PAL::LoadXcbWindowSystemAPI();
    const PAL::WaylandWindowSystemAPI& wayland = PAL::LoadWaylandWindowSystemAPI();
#    if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
    REQUIRE(x11.DeclaredCapabilities().windowSystem == Sora::Platform::WindowSystem::X11);
    REQUIRE(xcb.DeclaredCapabilities().windowSystem == Sora::Platform::WindowSystem::X11);
    REQUIRE(wayland.DeclaredCapabilities().windowSystem == Sora::Platform::WindowSystem::Wayland);
    REQUIRE(x11.DeclaredCapabilities().backend == PAL::NativeWindowBackend::Xlib);
    REQUIRE(xcb.DeclaredCapabilities().backend == PAL::NativeWindowBackend::Xcb);
    REQUIRE(wayland.DeclaredCapabilities().backend == PAL::NativeWindowBackend::Wayland);
    REQUIRE_FALSE(wayland.DeclaredCapabilities().Supports(PAL::WindowSystemFeature::WindowCreation));
    REQUIRE_FALSE(wayland.DeclaredCapabilities().Supports(PAL::WindowSystemFeature::VulkanSurface));
    // Library availability is deployment-dependent; no display server is required for table resolution.
    if (x11.Available()) {
        REQUIRE(x11.openDisplay != nullptr);
        REQUIRE(x11.pending != nullptr);
        REQUIRE(x11.nextEvent != nullptr);
    }
    if (xcb.Available()) {
        REQUIRE(xcb.connect != nullptr);
        REQUIRE(xcb.getFileDescriptor != nullptr);
        REQUIRE(xcb.pollForEvent != nullptr);
    }
    if (wayland.Available()) {
        REQUIRE(wayland.connect != nullptr);
        REQUIRE(wayland.dispatch != nullptr);
        REQUIRE(wayland.getFileDescriptor != nullptr);
    }
#    endif
    const PAL::EnvironmentSystemAPI& environment = PAL::LoadEnvironmentSystemAPI();
    REQUIRE(environment.get != nullptr);
    REQUIRE(environment.set != nullptr);
    REQUIRE(environment.remove != nullptr);
    REQUIRE((environment.environment != nullptr || environment.getEnvironment != nullptr));
    REQUIRE(environment.EnvironmentEntries() != nullptr);

    const PAL::ModuleSystemAPI& module = PAL::LoadModuleSystemAPI();
    REQUIRE(module.open != nullptr);
    REQUIRE(module.close != nullptr);
    REQUIRE(module.findSymbol != nullptr);

    REQUIRE(module.queryAddress != nullptr);
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
#    if defined(PLATFORM_LINUX)
    REQUIRE(thread.getThreadId != nullptr);
    REQUIRE(thread.getThreadId() > 0);
#    endif

    const PAL::FileSystemAPI& file = PAL::LoadFileSystemAPI();
    REQUIRE(file.open != nullptr);
    REQUIRE(file.close != nullptr);
    REQUIRE(file.write != nullptr);
    REQUIRE(file.sync != nullptr);
    const PAL::FileMappingSystemAPI& mapping = PAL::LoadFileMappingSystemAPI();
    REQUIRE(mapping.map != nullptr);
    REQUIRE(mapping.unmap != nullptr);
    REQUIRE(mapping.syncMap != nullptr);
    REQUIRE(mapping.adviseMap != nullptr);
    REQUIRE(PAL::LoadSystemInformationSystemAPI().systemConfiguration != nullptr);

    const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
    REQUIRE(stackTrace.captureStackBackTrace != nullptr);
#else
    SUCCEED("The unsupported-platform tables intentionally contain no entry points.");
#endif
}
