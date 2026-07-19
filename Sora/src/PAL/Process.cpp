/**
 * @file Process.cpp
 * @brief Implement current-process identity, startup metadata, and resource-usage introspection.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Process.h>

#include <Sora/Core/Guard.h>
#include <Sora/Core/PAL/File.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <array>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace Sora::PAL {

    namespace {

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        [[nodiscard]] Result<std::string> ReadProcessPseudoFile(const std::filesystem::path& path) {
            constexpr size_t kMaximumBytes = 16 * 1024 * 1024;
            auto opened = File::Open(path);
            if (!opened) {
                return std::unexpected(ErrorCode::ProcessNativeFailure);
            }

            std::string result;
            std::array<char, 4096> chunk{};
            uint64_t offset = 0;
            for (;;) {
                auto read = opened->ReadAt(std::as_writable_bytes(std::span{chunk}), offset);
                if (!read) {
                    return std::unexpected(ErrorCode::ProcessNativeFailure);
                }
                if (*read == 0) {
                    return result;
                }
                if (result.size() > kMaximumBytes - *read) {
                    return std::unexpected(ErrorCode::ProcessNativeFailure);
                }
                result.append(chunk.data(), *read);
                offset += *read;
            }
        }

        [[nodiscard]] Result<std::vector<std::string>> ParseNullSeparatedArguments(std::string_view bytes) {
            if (bytes.empty()) {
                return std::vector<std::string>{};
            }
            if (bytes.back() != '\0') {
                return std::unexpected(ErrorCode::InvalidNativeProcessText);
            }

            std::vector<std::string> arguments;
            size_t begin = 0;
            while (begin < bytes.size()) {
                const size_t end = bytes.find('\0', begin);
                if (end == std::string_view::npos) {
                    return std::unexpected(ErrorCode::InvalidNativeProcessText);
                }
                const std::string_view argument = bytes.substr(begin, end - begin);
                if (!Unicode::ValidateUtf8(argument)) {
                    return std::unexpected(ErrorCode::InvalidNativeProcessText);
                }
                arguments.emplace_back(argument);
                begin = end + 1;
            }
            return arguments;
        }
#endif

    } // namespace

    Result<ProcessId> CurrentProcessId() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (api.getCurrentProcessId == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const WindowsSystem::DWord id = api.getCurrentProcessId();
        return id != 0 ? Result<ProcessId>{static_cast<ProcessId>(id)}
                       : Result<ProcessId>{std::unexpected(ErrorCode::ProcessNativeFailure)};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.getProcessId)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int id = api.getProcessId();
        return id > 0 ? Result<ProcessId>{static_cast<ProcessId>(id)}
                      : Result<ProcessId>{std::unexpected(ErrorCode::ProcessNativeFailure)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<ProcessId> ParentProcessId() noexcept {
#if defined(PLATFORM_WINDOWS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.queryParentProcessId)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        WindowsSystem::DWord parent = 0;
        return api.queryParentProcessId(&parent) ? Result<ProcessId>{static_cast<ProcessId>(parent)}
                                                 : Result<ProcessId>{std::unexpected(ErrorCode::ProcessNativeFailure)};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.getParentProcessId)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int id = api.getParentProcessId();
        return id >= 0 ? Result<ProcessId>{static_cast<ProcessId>(id)}
                       : Result<ProcessId>{std::unexpected(ErrorCode::ProcessNativeFailure)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<std::filesystem::path> CurrentProcessImagePath() {
#if defined(PLATFORM_WINDOWS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.getCurrentProcess, api.queryFullProcessImageNameWide)) {
            return std::unexpected(ErrorCode::NotSupported);
        }

        std::wstring path(512, L'\0');
        while (path.size() <= 32'768) {
            WindowsSystem::DWord size = static_cast<WindowsSystem::DWord>(path.size());
            if (api.queryFullProcessImageNameWide(api.getCurrentProcess(), 0, path.data(), &size) != 0) {
                path.resize(size);
                return std::filesystem::path{std::move(path)};
            }
            path.resize(path.size() * 2);
        }
        return std::unexpected(ErrorCode::ProcessNativeFailure);
#elif defined(PLATFORM_LINUX)
        std::error_code error;
        std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error || path.empty()) {
            return Result<std::filesystem::path>{std::unexpected(ErrorCode::ProcessNativeFailure)};
        }
        return std::filesystem::path{std::move(path)};
#elif defined(PLATFORM_MACOS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (api.getExecutablePath == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        std::vector<char> path(1024);
        uint32_t size = static_cast<uint32_t>(path.size());
        if (api.getExecutablePath(path.data(), &size) != 0) {
            if (size == 0) {
                return std::unexpected(ErrorCode::ProcessNativeFailure);
            }
            path.resize(size);
            if (api.getExecutablePath(path.data(), &size) != 0) {
                return std::unexpected(ErrorCode::ProcessNativeFailure);
            }
        }
        return std::filesystem::path{path.data()};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<std::vector<std::string>> CurrentProcessArguments() {
#if defined(PLATFORM_WINDOWS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.getCommandLineWide, api.commandLineToArgvWide, api.localFree)) {
            return std::unexpected(ErrorCode::NotSupported);
        }

        const wchar_t* commandLine = api.getCommandLineWide();
        if (commandLine == nullptr) {
            return std::unexpected(ErrorCode::ProcessNativeFailure);
        }

        int count = 0;
        wchar_t** nativeArguments = api.commandLineToArgvWide(commandLine, &count);
        if (nativeArguments == nullptr || count < 0) {
            return std::unexpected(ErrorCode::ProcessNativeFailure);
        }

        Sora::ScopeExit releaseArguments{[&api, nativeArguments] noexcept { api.localFree(nativeArguments); }};

        // Convert each argument to UTF-8 and validate the result.
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            if (nativeArguments[index] == nullptr) {
                return std::unexpected(ErrorCode::InvalidNativeProcessText);
            }
            auto argument = Unicode::WideToUtf8(std::wstring_view{nativeArguments[index]});
            if (!argument) {
                return std::unexpected(ErrorCode::InvalidNativeProcessText);
            }
            arguments.push_back(std::move(*argument));
        }
        return arguments;
#elif defined(PLATFORM_LINUX)
        auto bytes = ReadProcessPseudoFile("/proc/self/cmdline");
        return bytes ? ParseNullSeparatedArguments(*bytes)
                     : Result<std::vector<std::string>>{std::unexpected(bytes.error())};
#elif defined(PLATFORM_MACOS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (api.getArgumentCount == nullptr || api.getArgumentVector == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        const int* count = api.getArgumentCount();
        char*** vectorAddress = api.getArgumentVector();
        if (count == nullptr || *count < 0 || vectorAddress == nullptr || *vectorAddress == nullptr) {
            return std::unexpected(ErrorCode::ProcessNativeFailure);
        }

        std::vector<std::string> arguments;
        arguments.reserve(static_cast<size_t>(*count));
        for (int index = 0; index < *count; ++index) {
            const char* nativeArgument = (*vectorAddress)[index];
            if (nativeArgument == nullptr || !Unicode::ValidateUtf8(nativeArgument)) {
                return std::unexpected(ErrorCode::InvalidNativeProcessText);
            }
            arguments.emplace_back(nativeArgument);
        }
        return arguments;
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<ProcessUsage> CaptureCurrentProcessUsage() noexcept {
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const ProcessSystemAPI& api = LoadProcessSystemAPI();
        if (!EnsureSystemAPIs(api.captureUsage)) {
            return std::unexpected(ErrorCode::NotSupported);
        }

        ProcessUsageCounters counters{};
        if (!api.captureUsage(&counters) ||
            counters.userCpuNanoseconds > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            counters.kernelCpuNanoseconds > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            counters.peakResidentMemoryBytes < counters.residentMemoryBytes) {
            return std::unexpected(ErrorCode::ProcessNativeFailure);
        }
        return ProcessUsage{
            .userCpuTime = std::chrono::nanoseconds{static_cast<int64_t>(counters.userCpuNanoseconds)},
            .kernelCpuTime = std::chrono::nanoseconds{static_cast<int64_t>(counters.kernelCpuNanoseconds)},
            .residentMemoryBytes = counters.residentMemoryBytes,
            .peakResidentMemoryBytes = counters.peakResidentMemoryBytes,
        };
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

} // namespace Sora::PAL
