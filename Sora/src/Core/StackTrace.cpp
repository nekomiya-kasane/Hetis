/**
 * @file StackTrace.cpp
 * @brief Native stack capture and symbol formatting implementation.
 * @ingroup Core
 */

#include "Sora/Core/StackTrace.h"

#include "Sora/Core/ABI.h"
#include "Sora/PAL/Module.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <mutex>

#ifdef PLATFORM_WINDOWS
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#    include <DbgHelp.h>
#else
#    include <dlfcn.h>
#    include <execinfo.h>
#endif

namespace Sora {

    namespace {

        /** @brief Return the basename component of @p path. */
        [[nodiscard]] std::string Basename(std::string_view path) {
            const size_t pos = path.find_last_of("\\/");
            return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
        }

        /** @brief Return @p text truncated to a diagnostic-friendly length. */
        [[nodiscard]] std::string Truncate(std::string_view text, size_t maxBytes) {
            if (text.size() <= maxBytes) {
                return std::string(text);
            }
            std::string out{text.substr(0, maxBytes > 3 ? maxBytes - 3 : maxBytes)};
            out += "...";
            return out;
        }

#ifdef PLATFORM_WINDOWS
        using SymSetOptionsFn = DWORD(__stdcall*)(DWORD);
        using SymInitializeFn = BOOL(__stdcall*)(HANDLE, PCSTR, BOOL);
        using SymFromAddrFn = BOOL(__stdcall*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
        using SymGetLineFromAddr64Fn = BOOL(__stdcall*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
        using SymGetModuleInfo64Fn = BOOL(__stdcall*)(HANDLE, DWORD64, PIMAGEHLP_MODULE64);

        /** @brief Lazily loaded DbgHelp entry points kept alive with the owning module. */
        struct DbgHelpApi {
            PAL::ModulePtr module{};
            SymSetOptionsFn symSetOptions = nullptr;
            SymInitializeFn symInitialize = nullptr;
            SymFromAddrFn symFromAddr = nullptr;
            SymGetLineFromAddr64Fn symGetLineFromAddr64 = nullptr;
            SymGetModuleInfo64Fn symGetModuleInfo64 = nullptr;

            /** @brief Return true when all required symbol APIs are available. */
            [[nodiscard]] explicit operator bool() const noexcept {
                return module && symSetOptions != nullptr && symInitialize != nullptr && symFromAddr != nullptr &&
                       symGetLineFromAddr64 != nullptr && symGetModuleInfo64 != nullptr;
            }
        };

        /** @brief Resolve DbgHelp only on first stack capture, avoiding a mandatory link-time dependency. */
        [[nodiscard]] DbgHelpApi& DbgHelp() noexcept {
            static DbgHelpApi api = [] {
                DbgHelpApi loaded{};
                auto module = PAL::LoadModule({"dbghelp"}, true);
                if (!module) {
                    return loaded;
                }
                loaded.module = std::move(*module);
                loaded.symSetOptions = loaded.module->TryFindSymbol<std::remove_pointer_t<SymSetOptionsFn>>(
                    "SymSetOptions");
                loaded.symInitialize = loaded.module->TryFindSymbol<std::remove_pointer_t<SymInitializeFn>>(
                    "SymInitialize");
                loaded.symFromAddr = loaded.module->TryFindSymbol<std::remove_pointer_t<SymFromAddrFn>>("SymFromAddr");
                loaded.symGetLineFromAddr64 =
                    loaded.module->TryFindSymbol<std::remove_pointer_t<SymGetLineFromAddr64Fn>>(
                        "SymGetLineFromAddr64");
                loaded.symGetModuleInfo64 = loaded.module->TryFindSymbol<std::remove_pointer_t<SymGetModuleInfo64Fn>>(
                    "SymGetModuleInfo64");
                return loaded;
            }();
            return api;
        }

        /** @brief DbgHelp is process-global and documented as requiring external synchronization. */
        [[nodiscard]] std::mutex& DbgHelpMutex() {
            static std::mutex mutex;
            return mutex;
        }

        /** @brief Initialize DbgHelp once when the API is available. */
        [[nodiscard]] bool EnsureDbgHelpInitialized() {
            static std::once_flag flag;
            static bool initialized = false;
            std::call_once(flag, [] {
                DbgHelpApi& api = DbgHelp();
                if (!api) {
                    return;
                }
                api.symSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                initialized = api.symInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
            });
            return initialized;
        }
#endif

    } // namespace

    std::string_view StackFrame::DisplaySymbol() const noexcept {
        return symbol.empty() ? std::string_view{"<unknown>"} : std::string_view{symbol};
    }

    StackTrace StackTrace::Capture(uint32_t skipFrames, uint32_t maxFrames) {
        maxFrames = std::min<uint32_t>(maxFrames, 128);
        if (maxFrames == 0) {
            return {};
        }

#ifdef PLATFORM_WINDOWS
        std::array<void*, 128> rawFrames{};
        const USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(skipFrames + 1), static_cast<DWORD>(maxFrames),
                                                      rawFrames.data(), nullptr);
        std::inplace_vector<StackFrame, 32> frames;
        frames.reserve(captured);

        if (!EnsureDbgHelpInitialized()) {
            for (USHORT i = 0; i < captured; ++i) {
                frames.push_back({.address = reinterpret_cast<uintptr_t>(rawFrames[i])});
            }
            return StackTrace{std::move(frames)};
        }

        std::lock_guard lock(DbgHelpMutex());
        DbgHelpApi& api = DbgHelp();
        HANDLE process = GetCurrentProcess();

        alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        for (USHORT i = 0; i < captured; ++i) {
            StackFrame frame{.address = reinterpret_cast<uintptr_t>(rawFrames[i])};
            const auto address = static_cast<DWORD64>(frame.address);

            DWORD64 displacement = 0;
            if (api.symFromAddr(process, address, &displacement, symbol) != FALSE) {
                frame.symbol = ABI::Demangle(std::string_view{symbol->Name, symbol->NameLen});
                frame.offset = static_cast<uint64_t>(displacement);
            }

            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (api.symGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE && line.FileName) {
                frame.sourceFile = line.FileName;
                frame.sourceLine = line.LineNumber;
            }

            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            if (api.symGetModuleInfo64(process, address, &module) != FALSE && module.ImageName[0] != '\0') {
                frame.module = Basename(module.ImageName);
            }

            frames.push_back(std::move(frame));
        }
        return StackTrace{std::move(frames)};
#else
        std::array<void*, 128> rawFrames{};
        const int requested = static_cast<int>(std::min<uint32_t>(skipFrames + 1 + maxFrames, rawFrames.size()));
        const int captured = backtrace(rawFrames.data(), requested);
        const int start = std::min<int>(captured, static_cast<int>(skipFrames + 1));

        std::vector<StackFrame> frames;
        frames.reserve(static_cast<size_t>(captured - start));
        for (int i = start; i < captured; ++i) {
            StackFrame frame{.address = reinterpret_cast<uintptr_t>(rawFrames[static_cast<size_t>(i)])};
            Dl_info info{};
            if (dladdr(rawFrames[static_cast<size_t>(i)], &info) != 0) {
                if (info.dli_sname != nullptr) {
                    frame.symbol = ABI::Demangle(info.dli_sname);
                    auto* frameAddress = reinterpret_cast<const char*>(rawFrames[static_cast<size_t>(i)]);
                    frame.offset = static_cast<uint64_t>(frameAddress - reinterpret_cast<const char*>(info.dli_saddr));
                }
                if (info.dli_fname != nullptr) {
                    frame.module = Basename(info.dli_fname);
                }
            }
            frames.push_back(std::move(frame));
        }
        return StackTrace{std::move(frames)};
#endif
    }

    std::string StackTrace::FormatFrame(const StackFrame& frame, size_t index) {
        const std::string symbol = Truncate(frame.DisplaySymbol(), 120);
        if (!frame.sourceFile.empty()) {
            return std::format("#{:<2} {} ({}:{} +0x{:X})", index, symbol, Basename(frame.sourceFile),
                               frame.sourceLine, frame.offset);
        }
        if (!frame.module.empty()) {
            return std::format("#{:<2} {} [{}+0x{:X}]", index, symbol, frame.module, frame.offset);
        }
        return std::format("#{:<2} {} (0x{:X})", index, symbol, frame.address);
    }

    std::string StackTrace::ToString() const {
        std::string out;
        out.reserve(frames_.size() * 128 + 32);
        out += std::format("StackTrace({} frames)", frames_.size());
        for (size_t i = 0; i < frames_.size(); ++i) {
            out += "\n  ";
            out += FormatFrame(frames_[i], i);
        }
        return out;
    }

    void StackTrace::ToStyledString(Sora::$::Serialization::StyledStringBuilder& builder) const {
        namespace Styled = Sora::$::Serialization;
        builder.Text(Styled::StyledRole::TypeName, "StackTrace");
        builder.Raw(Styled::StyledRole::Punctuation, "(");
        builder.Text(Styled::StyledRole::Number, std::format("{}", frames_.size()));
        builder.Raw(Styled::StyledRole::Plain, " frames");
        builder.Raw(Styled::StyledRole::Punctuation, ")");
        for (size_t i = 0; i < frames_.size(); ++i) {
            const StackFrame& frame = frames_[i];
            builder.Raw("\n  ");
            builder.Text(Styled::StyledRole::FieldName, std::format("#{:<2}", i));
            builder.Raw(Styled::StyledRole::Plain, " ");
            builder.Text(frame.symbol.empty() ? Styled::StyledRole::Null : Styled::StyledRole::TypeName,
                         Truncate(frame.DisplaySymbol(), 120));
            if (!frame.sourceFile.empty()) {
                builder.Raw(Styled::StyledRole::Plain, " (");
                builder.Text(Styled::StyledRole::String, Basename(frame.sourceFile));
                builder.Raw(Styled::StyledRole::Punctuation, ":");
                builder.Text(Styled::StyledRole::Number, std::format("{}", frame.sourceLine));
                builder.Raw(Styled::StyledRole::Plain, std::format(" +0x{:X})", frame.offset));
            } else if (!frame.module.empty()) {
                builder.Raw(Styled::StyledRole::Plain, " [");
                builder.Text(Styled::StyledRole::String, frame.module);
                builder.Raw(Styled::StyledRole::Plain, std::format("+0x{:X}]", frame.offset));
            } else {
                builder.Raw(Styled::StyledRole::Plain, std::format(" (0x{:X})", frame.address));
            }
        }
    }

} // namespace Sora
