/**
 * @file InterprocessFileLockProcessTest.cpp
 * @brief Verify that forced process termination releases an owned file lock.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/InterprocessFileLock.h>
#include <Sora/Platform.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#    include <process.h>
#else
#    include <csignal>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace {

    constexpr auto kChildStartupTimeout = std::chrono::seconds{5};
    constexpr auto kPollInterval = std::chrono::milliseconds{1};

    int RunChild(const std::filesystem::path& lockPath, const std::filesystem::path& readyPath) {
        auto acquired = Sora::PAL::InterprocessFileLock::TryAcquire(lockPath);
        if (!acquired || !*acquired) {
            return 2;
        }
        std::ofstream ready{readyPath, std::ios::binary | std::ios::trunc};
        ready.put('1');
        ready.close();
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return 3;
    }

    [[nodiscard]] bool WaitForReady(const std::filesystem::path& readyPath) {
        const auto deadline = std::chrono::steady_clock::now() + kChildStartupTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(readyPath)) {
                return true;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        return false;
    }

#if defined(PLATFORM_WINDOWS)
    [[nodiscard]] HANDLE SpawnChild(const std::filesystem::path& executable, const std::filesystem::path& lockPath,
                                    const std::filesystem::path& readyPath) {
        const intptr_t process = ::_wspawnl(_P_NOWAIT, executable.c_str(), executable.c_str(), L"--lock-child",
                                            lockPath.c_str(), readyPath.c_str(), static_cast<const wchar_t*>(nullptr));
        return process == -1 ? nullptr : reinterpret_cast<HANDLE>(process);
    }

    [[nodiscard]] bool TerminateAndWait(HANDLE process) {
        const bool terminated = ::TerminateProcess(process, 73) != FALSE;
        const bool waited = ::WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
        ::CloseHandle(process);
        return terminated && waited;
    }
#else
    [[nodiscard]] pid_t SpawnChild(const std::filesystem::path& executable, const std::filesystem::path& lockPath,
                                   const std::filesystem::path& readyPath) {
        const pid_t process = ::fork();
        if (process == 0) {
            const std::string executableText = executable.string();
            const std::string lockText = lockPath.string();
            const std::string readyText = readyPath.string();
            ::execl(executableText.c_str(), executableText.c_str(), "--lock-child", lockText.c_str(), readyText.c_str(),
                    nullptr);
            ::_exit(126);
        }
        return process;
    }

    [[nodiscard]] bool TerminateAndWait(pid_t process) {
        if (process <= 0 || ::kill(process, SIGKILL) != 0) {
            return false;
        }
        int status = 0;
        return ::waitpid(process, &status, 0) == process && WIFSIGNALED(status);
    }
#endif

    int RunParent(const std::filesystem::path& executable) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / ("sora-file-lock-process-test-" + std::to_string(stamp));
        const std::filesystem::path lockPath = directory / "crash-release.lock";
        const std::filesystem::path readyPath = directory / "ready";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return 1;
        }

        const auto child = SpawnChild(std::filesystem::absolute(executable), lockPath, readyPath);
#if defined(PLATFORM_WINDOWS)
        if (child == nullptr) {
#else
        if (child <= 0) {
#endif
            std::filesystem::remove_all(directory, error);
            return 1;
        }
        if (!WaitForReady(readyPath)) {
            static_cast<void>(TerminateAndWait(child));
            std::filesystem::remove_all(directory, error);
            return 1;
        }

        auto contended = Sora::PAL::InterprocessFileLock::TryAcquire(lockPath);
        const bool excluded = contended && !*contended;
        const bool terminated = TerminateAndWait(child);
        auto released = Sora::PAL::InterprocessFileLock::TryAcquire(lockPath);
        const bool reacquired = released && released->has_value();
        released = std::unexpected(Sora::ErrorCode::IoError);
        std::filesystem::remove_all(directory, error);
        return excluded && terminated && reacquired ? 0 : 1;
    }

} // namespace

#if defined(PLATFORM_WINDOWS)
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view{argv[1]} == L"--lock-child") {
        return RunChild(std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]});
    }
    return argc >= 1 ? RunParent(std::filesystem::path{argv[0]}) : 1;
}
#else
int main(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--lock-child") {
        return RunChild(std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]});
    }
    return argc >= 1 ? RunParent(std::filesystem::path{argv[0]}) : 1;
}
#endif
