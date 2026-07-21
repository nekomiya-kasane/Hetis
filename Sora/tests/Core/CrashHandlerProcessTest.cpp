/**
 * @file CrashHandlerProcessTest.cpp
 * @brief Verify fatal crash recording in an isolated child process.
 * @ingroup Testing
 */

#include <Sora/Core/CrashHandler.h>
#include <Sora/Platform.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

    void WriteApplicationState(const Sora::CrashContext&, Sora::CrashStream stream) noexcept {
        static_cast<void>(stream.Write("callback-marker\n"));
    }

    [[noreturn]] void TriggerFatalCrash() {
#if defined(PLATFORM_WINDOWS)
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        ::TerminateProcess(::GetCurrentProcess(), 127);
        std::abort();
#else
        ::raise(SIGABRT);
        ::_exit(127);
#endif
    }

    int RunChild(const std::filesystem::path& dumpPath) {
        const Sora::CrashHandlerOptions options{
            .emergencyFile = dumpPath,
            .callback = WriteApplicationState,
            .mirrorToStandardError = false,
        };
        auto handler = Sora::CrashHandler::Install(options);
        if (!handler) {
            return 2;
        }
        TriggerFatalCrash();
    }

    [[nodiscard]] int SpawnChild(const std::filesystem::path& executable, const std::filesystem::path& dumpPath) {
#if defined(PLATFORM_WINDOWS)
        const std::wstring& executableText = executable.native();
        const std::wstring& dumpText = dumpPath.native();
        return static_cast<int>(::_wspawnl(_P_WAIT, executableText.c_str(), executableText.c_str(), L"--crash-child",
                                           dumpText.c_str(), static_cast<const wchar_t*>(nullptr)));
#else
        const pid_t process = ::fork();
        if (process == 0) {
            const std::string executableText = executable.string();
            const std::string dumpText = dumpPath.string();
            ::execl(executableText.c_str(), executableText.c_str(), "--crash-child", dumpText.c_str(), nullptr);
            ::_exit(126);
        }
        if (process < 0) {
            return -1;
        }
        int status = 0;
        return ::waitpid(process, &status, 0) == process ? status : -1;
#endif
    }

    int RunParent(const std::filesystem::path& executable) {
        const std::filesystem::path dumpPath =
            std::filesystem::temp_directory_path() / L"sora-crash-handler-process-test.txt";
        std::error_code cleanupError;
        std::filesystem::remove(dumpPath, cleanupError);

        const int childStatus = SpawnChild(std::filesystem::absolute(executable), dumpPath);
        if (childStatus == 0 || !std::filesystem::exists(dumpPath)) {
            return 1;
        }

        std::ifstream input{dumpPath, std::ios::binary};
        const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        std::filesystem::remove(dumpPath, cleanupError);
        if (!contents.contains("=== SORA CRASH RECORD ===") || !contents.contains("Fault address: 0x") ||
            !contents.contains("Instruction pointer: 0x") || !contents.contains("callback-marker") ||
            !contents.contains("=== END SORA CRASH RECORD ===")) {
            std::fwrite(contents.data(), 1, contents.size(), stderr);
            return 1;
        }
        return 0;
    }

} // namespace

#if defined(PLATFORM_WINDOWS)
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view{argv[1]} == L"--crash-child") {
        return RunChild(std::filesystem::path{argv[2]});
    }
    return argc >= 1 ? RunParent(std::filesystem::path{argv[0]}) : 1;
}
#else
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view{argv[1]} == "--crash-child") {
        return RunChild(std::filesystem::path{argv[2]});
    }
    return argc >= 1 ? RunParent(std::filesystem::path{argv[0]}) : 1;
}
#endif
