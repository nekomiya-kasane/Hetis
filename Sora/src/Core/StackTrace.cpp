/**
 * @file StackTrace.cpp
 * @brief Native stack capture and symbol formatting implementation.
 * @details Use @ref Sora::StackTrace::Capture from ordinary diagnostic paths. Fatal handlers must use
 * @ref Sora::CrashHandler instead because native symbol resolution allocates and requires process-global locks.
 * @ingroup Core
 */

#include "Sora/Core/StackTrace.h"

#include "Sora/Core/ABI.h"
#include "Sora/Core/Path.h"
#include "Sora/Core/PAL/SystemAPI.h"
#include "Sora/Core/StringUtils.h"
#include "Sora/Core/ToStyledString.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <limits>

#ifdef PLATFORM_WINDOWS
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#    include <DbgHelp.h>
#endif

namespace Sora {

    namespace {

#ifdef PLATFORM_WINDOWS
        /** @brief Return whether every DbgHelp entry point required for full frame symbolization is available. */
        [[nodiscard]] bool HasRequiredDbgHelpAPI(const PAL::DbgHelpSystemAPI& api) noexcept {
            return api.symSetOptions != nullptr && api.symInitialize != nullptr && api.symFromAddress != nullptr &&
                   api.symGetLineFromAddress != nullptr && api.symGetModuleInfo != nullptr;
        }

#endif

    } // namespace

    std::string_view StackFrame::DisplaySymbol() const noexcept {
        return symbol.empty() ? std::string_view{"<unknown>"} : std::string_view{symbol};
    }

    StackTrace StackTrace::Capture(StackTraceCaptureOptions options) {
        const uint32_t skipFrames = options.skipFrames;
        uint32_t maxFrames = options.maxFrames;
        maxFrames = std::min<uint32_t>(maxFrames, static_cast<uint32_t>(StackTrace::kMaximumFrameCount));
        if (maxFrames == 0) {
            return {};
        }

#ifdef PLATFORM_WINDOWS
        if (skipFrames == std::numeric_limits<uint32_t>::max()) {
            return {};
        }
        const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
        if (stackTrace.captureStackBackTrace == nullptr) {
            return {};
        }
        std::array<void*, 128> rawFrames{};
        const auto nativeSkipFrames = static_cast<PAL::WindowsSystem::DWord>(uint64_t{skipFrames} + 1);
        const uint16_t captured = stackTrace.captureStackBackTrace(
            nativeSkipFrames, static_cast<PAL::WindowsSystem::DWord>(maxFrames), rawFrames.data(), nullptr);
        StackTrace::ContainerType frames;
        frames.reserve(captured);

        if (!PAL::InitializeDbgHelpSystemAPI()) {
            for (uint16_t i = 0; i < captured; ++i) {
                frames.push_back({.address = reinterpret_cast<uintptr_t>(rawFrames[i])});
            }
            return StackTrace{std::move(frames)};
        }

        {
            PAL::LockedDbgHelpSystemAPI dbgHelp = PAL::LockDbgHelpSystemAPI();
            if (!HasRequiredDbgHelpAPI(*dbgHelp)) {
                for (uint16_t i = 0; i < captured; ++i) {
                    frames.push_back({.address = reinterpret_cast<uintptr_t>(rawFrames[i])});
                }
                return StackTrace{std::move(frames)};
            }

            PAL::WindowsSystem::Handle process = stackTrace.getCurrentProcess();
            alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;

            for (uint16_t i = 0; i < captured; ++i) {
                StackFrame frame{.address = reinterpret_cast<uintptr_t>(rawFrames[i])};
                const auto address = static_cast<DWORD64>(frame.address);

                DWORD64 displacement = 0;
                if (dbgHelp->symFromAddress(process, address, &displacement, symbol) != FALSE) {
                    frame.symbol.assign(symbol->Name, symbol->NameLen);
                    frame.offset = static_cast<uint64_t>(displacement);
                }

                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                PAL::WindowsSystem::DWord lineDisplacement = 0;
                if (dbgHelp->symGetLineFromAddress(process, address, &lineDisplacement, &line) != FALSE &&
                    line.FileName != nullptr) {
                    frame.sourceFile = line.FileName;
                    frame.sourceLine = line.LineNumber;
                }

                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                if (dbgHelp->symGetModuleInfo(process, address, &module) != FALSE && module.ImageName[0] != '\0') {
                    frame.module = Sora::FileName(module.ImageName);
                }

                frames.push_back(std::move(frame));
            }
        }
        for (StackFrame& frame : frames) {
            if (!frame.symbol.empty()) {
                frame.symbol = ABI::Demangle(frame.symbol);
            }
        }
        return StackTrace{std::move(frames)};
#else
        const PAL::StackTraceSystemAPI& stackTrace = PAL::LoadStackTraceSystemAPI();
        if (stackTrace.captureStackBackTrace == nullptr) {
            return {};
        }
        std::array<void*, 128> rawFrames{};
        const uint64_t requestedFrames = uint64_t{skipFrames} + 1 + maxFrames;
        const int requested = static_cast<int>(std::min<uint64_t>(requestedFrames, rawFrames.size()));
        const int captured = stackTrace.captureStackBackTrace(rawFrames.data(), requested);
        if (captured <= 0) {
            return {};
        }
        const int start =
            static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(captured), uint64_t{skipFrames} + 1));

        StackTrace::ContainerType frames;
        frames.reserve(static_cast<size_t>(captured - start));
        for (int i = start; i < captured; ++i) {
            StackFrame frame{.address = reinterpret_cast<uintptr_t>(rawFrames[static_cast<size_t>(i)])};
            PAL::DynamicSymbolInfo info{};
            if (stackTrace.findDynamicSymbol != nullptr &&
                stackTrace.findDynamicSymbol(rawFrames[static_cast<size_t>(i)], &info) != 0) {
                if (info.symbolName != nullptr) {
                    frame.symbol = ABI::Demangle(info.symbolName);
                    const uintptr_t symbolAddress = reinterpret_cast<uintptr_t>(info.symbolAddress);
                    frame.offset = frame.address >= symbolAddress ? frame.address - symbolAddress : 0;
                }
                if (info.fileName != nullptr) {
                    frame.module = Sora::FileName(info.fileName);
                }
            }
            frames.push_back(std::move(frame));
        }
        return StackTrace{std::move(frames)};
#endif
    }

    std::string StackTrace::FormatFrame(const StackFrame& frame, size_t index) {
        const std::string symbol = Ascii::String::Truncate(frame.DisplaySymbol(), 120);
        if (!frame.sourceFile.empty()) {
            return std::format("#{:<2} {} ({}:{} +0x{:X})", index, symbol, Sora::FileName(frame.sourceFile),
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

    void StackTrace::ToStyledString(Sora::Styled::StyledStringBuilder& builder) const {
        namespace Styled = Sora::Styled;
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
                         Ascii::String::Truncate(frame.DisplaySymbol(), 120));
            if (!frame.sourceFile.empty()) {
                builder.Raw(Styled::StyledRole::Plain, " (");
                builder.Text(Styled::StyledRole::String, Sora::FileName(frame.sourceFile));
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
