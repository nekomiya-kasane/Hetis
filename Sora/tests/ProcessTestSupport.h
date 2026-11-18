/**
 * @file ProcessTestSupport.h
 * @brief Platform-neutral child-process primitives shared by isolated process tests.
 * @ingroup Testing
 */
#pragma once

#include <Sora/Core/PAL/SystemAPI.h>

#include <cerrno>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Sora::Test {

    class ChildProcess {
    public:
#ifdef PLATFORM_WINDOWS
        using NativeHandle = PAL::WindowsSystem::Handle;
        inline static constexpr NativeHandle kInvalidHandle = nullptr;
#else
        using NativeHandle = PAL::PosixSystem::ProcessId;
        inline static constexpr NativeHandle kInvalidHandle = -1;
#endif

        ChildProcess() = default;
        explicit ChildProcess(NativeHandle handle) noexcept : handle_{handle} {}
        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        ChildProcess(ChildProcess&& other) noexcept : handle_{std::exchange(other.handle_, kInvalidHandle)} {}

        ChildProcess& operator=(ChildProcess&& other) noexcept {
            if (this != &other) {
                Reset();
                handle_ = std::exchange(other.handle_, kInvalidHandle);
            }
            return *this;
        }

        ~ChildProcess() { Reset(); }

        [[nodiscard]] bool Valid() const noexcept { return handle_ != kInvalidHandle; }
        [[nodiscard]] NativeHandle Get() const noexcept { return handle_; }

        void Reset() noexcept {
#ifdef PLATFORM_WINDOWS
            if (Valid()) {
                if (const auto& handles = PAL::LoadHandleSystemAPI(); handles.closeHandle != nullptr) {
                    handles.closeHandle(handle_);
                }
            }
#endif
            handle_ = kInvalidHandle;
        }

    private:
        NativeHandle handle_ = kInvalidHandle;
    };

#ifdef PLATFORM_WINDOWS
    [[nodiscard]] inline std::wstring QuoteWindowsArgument(std::wstring_view argument) {
        if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
            return std::wstring{argument};
        }

        std::wstring quoted{L'"'};
        size_t backslashCount = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashCount;
                continue;
            }
            if (character == L'"') {
                quoted.append(backslashCount * 2 + 1, L'\\');
                quoted.push_back(character);
                backslashCount = 0;
                continue;
            }
            quoted.append(backslashCount, L'\\');
            backslashCount = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashCount * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }
#endif

    [[nodiscard]] inline std::optional<ChildProcess> SpawnChildProcess(
        const std::filesystem::path& executable, std::initializer_list<std::filesystem::path> arguments,
        const std::optional<std::filesystem::path>& standardErrorPath = std::nullopt) {
#ifdef PLATFORM_WINDOWS
        const auto& process = PAL::LoadProcessControlSystemAPI();
        const auto& files = PAL::LoadFileSystemAPI();
        const auto& handles = PAL::LoadHandleSystemAPI();
        if (!PAL::EnsureSystemAPIs(process.createProcessWide, files.getStandardHandle, files.createFileWide,
                                   handles.closeHandle)) {
            return std::nullopt;
        }

        std::wstring commandLine = QuoteWindowsArgument(executable.native());
        for (const auto& argument : arguments) {
            commandLine.push_back(L' ');
            commandLine += QuoteWindowsArgument(argument.native());
        }

        PAL::WindowsSystem::Handle standardError = nullptr;
        PAL::WindowsSystem::SecurityAttributes attributes{
            .length = sizeof(attributes),
            .securityDescriptor = nullptr,
            .inheritHandle = PAL::WindowsSystem::kTrue,
        };
        if (standardErrorPath) {
            standardError = files.createFileWide(
                standardErrorPath->c_str(), PAL::WindowsSystem::kGenericWrite,
                PAL::WindowsSystem::kFileShareRead | PAL::WindowsSystem::kFileShareWrite,
                reinterpret_cast<::_SECURITY_ATTRIBUTES*>(&attributes), PAL::WindowsSystem::kCreateAlways,
                PAL::WindowsSystem::kFileAttributeNormal, nullptr);
            if (PAL::WindowsSystem::IsInvalidHandle(standardError)) {
                return std::nullopt;
            }
        }

        PAL::WindowsSystem::StartupInfo startup{};
        startup.flags = standardErrorPath ? PAL::WindowsSystem::kStartUseStandardHandles : 0;
        startup.standardInput = files.getStandardHandle(PAL::WindowsSystem::kStandardInputHandle);
        startup.standardOutput = files.getStandardHandle(PAL::WindowsSystem::kStandardOutputHandle);
        startup.standardError = standardErrorPath ? standardError
                                                  : files.getStandardHandle(PAL::WindowsSystem::kStandardErrorHandle);
        PAL::WindowsSystem::ProcessInformation result{};
        const auto created = process.createProcessWide(
            executable.c_str(), commandLine.data(), nullptr, nullptr,
            standardErrorPath ? PAL::WindowsSystem::kTrue : PAL::WindowsSystem::kFalse,
            PAL::WindowsSystem::kCreateNoWindow, nullptr, nullptr, reinterpret_cast<::_STARTUPINFOW*>(&startup),
            reinterpret_cast<::_PROCESS_INFORMATION*>(&result));
        if (standardError != nullptr) {
            handles.closeHandle(standardError);
        }
        if (created == PAL::WindowsSystem::kFalse) {
            return std::nullopt;
        }
        handles.closeHandle(result.thread);
        return ChildProcess{result.process};
#else
        const auto& process = PAL::LoadProcessControlSystemAPI();
        const auto& files = PAL::LoadFileSystemAPI();
        if (!PAL::EnsureSystemAPIs(process.fork, process.execPath, files.open, files.close, files.duplicateTo)) {
            return std::nullopt;
        }

        std::vector<std::string> argumentStorage;
        argumentStorage.reserve(arguments.size() + 1);
        argumentStorage.push_back(executable.string());
        for (const auto& argument : arguments) {
            argumentStorage.push_back(argument.string());
        }
        std::vector<char*> argumentPointers;
        argumentPointers.reserve(argumentStorage.size() + 1);
        for (auto& argument : argumentStorage) {
            argumentPointers.push_back(argument.data());
        }
        argumentPointers.push_back(nullptr);

        std::optional<std::string> standardErrorStorage;
        if (standardErrorPath) {
            standardErrorStorage.emplace(standardErrorPath->string());
        }

        const PAL::PosixSystem::ProcessId child = process.fork();
        if (child == 0) {
            if (standardErrorStorage) {
                const int descriptor = files.open(standardErrorStorage->c_str(), PAL::PosixSystem::kOpenWriteOnly |
                                                                                     PAL::PosixSystem::kOpenCreate |
                                                                                     PAL::PosixSystem::kOpenTruncate,
                                                  PAL::PosixSystem::FileMode{0600});
                if (descriptor < 0 || files.duplicateTo(descriptor, PAL::PosixSystem::kStandardErrorDescriptor) < 0) {
                    std::_Exit(125);
                }
                files.close(descriptor);
            }
            process.execPath(argumentStorage.front().c_str(), argumentPointers.data());
            std::_Exit(126);
        }
        return child > 0 ? std::optional<ChildProcess>{std::in_place, child} : std::nullopt;
#endif
    }

    [[nodiscard]] inline std::optional<int> WaitChildProcess(ChildProcess& child) noexcept {
        if (!child.Valid()) {
            return std::nullopt;
        }
#ifdef PLATFORM_WINDOWS
        const auto& process = PAL::LoadProcessControlSystemAPI();
        const auto& synchronization = PAL::LoadSynchronizationSystemAPI();
        if (!PAL::EnsureSystemAPIs(process.getExitCode, synchronization.waitForSingleObject) ||
            synchronization.waitForSingleObject(child.Get(), PAL::WindowsSystem::kInfinite) !=
                PAL::WindowsSystem::kWaitObject) {
            return std::nullopt;
        }
        PAL::WindowsSystem::DWord exitCode = 0;
        if (process.getExitCode(child.Get(), &exitCode) == PAL::WindowsSystem::kFalse) {
            return std::nullopt;
        }
        child.Reset();
        return static_cast<int>(exitCode);
#else
        const auto& process = PAL::LoadProcessControlSystemAPI();
        if (process.wait == nullptr) {
            return std::nullopt;
        }
        int status = 0;
        PAL::PosixSystem::ProcessId waited = -1;
        do {
            waited = process.wait(child.Get(), &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != child.Get()) {
            return std::nullopt;
        }
        child.Reset();
        if (PAL::PosixSystem::ProcessExited(status)) {
            return PAL::PosixSystem::ProcessExitStatus(status);
        }
        if (PAL::PosixSystem::ProcessSignaled(status)) {
            return 128 + PAL::PosixSystem::ProcessTerminationSignal(status);
        }
        return std::nullopt;
#endif
    }

    [[nodiscard]] inline bool TerminateChildProcess(ChildProcess& child, uint32_t exitCode = 73) noexcept {
        if (!child.Valid()) {
            return false;
        }
#ifdef PLATFORM_WINDOWS
        const auto& process = PAL::LoadProcessControlSystemAPI();
        if (process.terminate == nullptr ||
            process.terminate(child.Get(), static_cast<PAL::WindowsSystem::UInt>(exitCode)) ==
                PAL::WindowsSystem::kFalse) {
            return false;
        }
#else
        const auto& process = PAL::LoadProcessControlSystemAPI();
        if (process.kill == nullptr || process.kill(child.Get(), PAL::PosixSystem::kKillSignal) != 0) {
            return false;
        }
#endif
        return WaitChildProcess(child).has_value();
    }

} // namespace Sora::Test
