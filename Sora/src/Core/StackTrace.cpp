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
#include <format>
#include <limits>

namespace Sora {

    std::string_view StackFrame::DisplaySymbol() const noexcept {
        return symbol.empty() ? std::string_view{"<unknown>"} : std::string_view{symbol};
    }

    StackTrace StackTrace::Capture(StackTraceCaptureOptions options) {
        const uint32_t skipFrames = options.skipFrames;
        const uint32_t maxFrames =
            std::min<uint32_t>(options.maxFrames, static_cast<uint32_t>(StackTrace::kMaximumFrameCount));
        if (maxFrames == 0 || skipFrames == std::numeric_limits<uint32_t>::max()) {
            return {};
        }

#ifdef PLATFORM_WINDOWS
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
        for (uint16_t index = 0; index < captured; ++index) {
            frames.push_back({.address = reinterpret_cast<uintptr_t>(rawFrames[index])});
        }

        const PAL::ProcessSystemAPI& process = PAL::LoadProcessSystemAPI();
        if (PAL::InitializeDbgHelpSystemAPI() && PAL::EnsureSystemAPIs(process.getCurrentProcess)) {
            void* nativeProcess = process.getCurrentProcess();
            if (nativeProcess != nullptr) {
                PAL::LockedDbgHelpSystemAPI dbgHelp = PAL::LockDbgHelpSystemAPI();
                for (StackFrame& frame : frames) {
                    PAL::DbgHelpAddressInfo info;
                    if (!dbgHelp.ResolveAddress(nativeProcess, frame.address, info)) {
                        continue;
                    }
                    frame.symbol = std::move(info.symbol);
                    frame.sourceFile = std::move(info.sourceFile);
                    frame.sourceLine = info.sourceLine;
                    frame.offset = info.offset;
                    if (!info.modulePath.empty()) {
                        frame.module = Sora::FileName(info.modulePath);
                    }
                }
            }
        }
        for (StackFrame& frame : frames) {
            if (!frame.symbol.empty()) {
                frame.symbol = ABI::Demangle(frame.symbol);
            }
        }
        return StackTrace{std::move(frames)};
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
        const PAL::ModuleSystemAPI& module = PAL::LoadModuleSystemAPI();
        for (int i = start; i < captured; ++i) {
            StackFrame frame{.address = reinterpret_cast<uintptr_t>(rawFrames[static_cast<size_t>(i)])};
            PAL::ModuleAddressInfo info{};
            if (module.queryAddress != nullptr && module.queryAddress(rawFrames[static_cast<size_t>(i)], &info)) {
                if (info.symbolName != nullptr) {
                    frame.symbol = ABI::Demangle(info.symbolName);
                    const uintptr_t symbolAddress = reinterpret_cast<uintptr_t>(info.symbolAddress);
                    frame.offset = frame.address >= symbolAddress ? frame.address - symbolAddress : 0;
                }
                if (info.modulePath != nullptr) {
                    frame.module = Sora::FileName(info.modulePath);
                }
            }
            frames.push_back(std::move(frame));
        }
        return StackTrace{std::move(frames)};
#else
        return {};
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
