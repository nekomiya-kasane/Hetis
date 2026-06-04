/**
 * @file StackTrace.cpp
 * @brief Cross-platform stack trace capture + tapioca-styled pretty-print.
 *
 * - Windows: CaptureStackBackTrace → DbgHelp (SymFromAddr, SymGetLineFromAddr64)
 * - POSIX:   backtrace → dladdr + abi::__cxa_demangle
 */

#include "Mashiro/Core/StackTrace.h"

#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <tapioca/console.h>
#include <tapioca/style.h>

// =============================================================================
// Platform includes
// =============================================================================

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <dbghelp.h>
#else
#    include <cxxabi.h>
#    include <dlfcn.h>
#    include <execinfo.h>
#endif

namespace Mashiro {

    // =========================================================================
    // Platform helpers
    // =========================================================================

    namespace {

        /// @brief Extract basename from a full path.
        std::string ExtractBasename(const char* path) {
            if (!path || !*path) return {};
            std::string_view sv{path};
            auto pos = sv.find_last_of("\\/");
            return std::string{(pos != std::string_view::npos) ? sv.substr(pos + 1) : sv};
        }

    } // anonymous namespace

    // =========================================================================
    // Windows: CaptureStackBackTrace + DbgHelp
    // =========================================================================

#ifdef _WIN32

    namespace {
        std::once_flag g_symInitFlag;
        std::mutex g_dbgHelpMutex;

        void EnsureSymbolsInitialized() {
            std::call_once(g_symInitFlag, [] {
                SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            });
        }
    } // anonymous namespace

    StackTrace StackTrace::Capture(uint32_t skipFrames, uint32_t maxFrames) {
        EnsureSymbolsInitialized();

        uint32_t totalSkip = skipFrames + 1; // +1 to skip Capture() itself
        uint32_t totalCapture = totalSkip + maxFrames;
        if (totalCapture > 128) totalCapture = 128;

        void* rawFrames[128];
        USHORT captured = CaptureStackBackTrace(
            static_cast<DWORD>(totalSkip),
            static_cast<DWORD>(maxFrames),
            rawFrames, nullptr);

        StackTrace trace;
        trace.frames_.reserve(captured);

        std::lock_guard lock(g_dbgHelpMutex);
        HANDLE proc = GetCurrentProcess();

        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;

        IMAGEHLP_LINE64 lineInfo{};
        lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        IMAGEHLP_MODULE64 modInfo{};
        modInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

        for (USHORT i = 0; i < captured; ++i) {
            StackFrame frame;
            frame.address = rawFrames[i];
            auto addr = reinterpret_cast<DWORD64>(rawFrames[i]);

            DWORD64 symDisp = 0;
            if (SymFromAddr(proc, addr, &symDisp, sym)) {
                frame.symbol = std::string(sym->Name, sym->NameLen);
                frame.offset = static_cast<uint64_t>(symDisp);
            }

            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &lineDisp, &lineInfo)) {
                frame.sourceFile = lineInfo.FileName;
                frame.sourceLine = lineInfo.LineNumber;
            }

            if (SymGetModuleInfo64(proc, addr, &modInfo)) {
                frame.module = ExtractBasename(modInfo.ImageName);
            }

            trace.frames_.push_back(std::move(frame));
        }

        return trace;
    }

#else // POSIX

    namespace {
        std::string DemangleSymbol(const char* mangled) {
            if (!mangled || !*mangled) return {};
            int status = 0;
            char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
            std::string result = (status == 0 && demangled) ? demangled : mangled;
            free(demangled);
            return result;
        }
    } // anonymous namespace

    StackTrace StackTrace::Capture(uint32_t skipFrames, uint32_t maxFrames) {
        uint32_t totalSkip = skipFrames + 1;
        uint32_t totalCapture = totalSkip + maxFrames;
        if (totalCapture > 128) totalCapture = 128;

        void* rawFrames[128];
        int captured = backtrace(rawFrames, static_cast<int>(totalCapture));

        StackTrace trace;
        int start = static_cast<int>(totalSkip);
        if (start > captured) start = captured;
        trace.frames_.reserve(static_cast<size_t>(captured - start));

        for (int i = start; i < captured; ++i) {
            StackFrame frame;
            frame.address = rawFrames[i];

            Dl_info info{};
            if (dladdr(rawFrames[i], &info)) {
                if (info.dli_sname) {
                    frame.symbol = DemangleSymbol(info.dli_sname);
                    frame.offset = static_cast<uint64_t>(
                        reinterpret_cast<const char*>(rawFrames[i]) -
                        reinterpret_cast<const char*>(info.dli_saddr));
                }
                if (info.dli_fname) {
                    frame.module = ExtractBasename(info.dli_fname);
                }
            }
            trace.frames_.push_back(std::move(frame));
        }

        return trace;
    }

#endif // _WIN32

    // =========================================================================
    // FormatCompact — single line per frame
    // =========================================================================

    std::string StackTrace::FormatCompact(const StackFrame& frame, uint32_t index) {
        std::string sym = frame.symbol.empty() ? "<unknown>" : frame.symbol;

        constexpr size_t kMaxSymLen = 80;
        if (sym.size() > kMaxSymLen) {
            sym.resize(kMaxSymLen - 1);
            sym += "\u2026"; // …
        }

        if (!frame.sourceFile.empty()) {
            auto file = std::string_view{frame.sourceFile};
            auto pos = file.find_last_of("\\/");
            if (pos != std::string_view::npos) file = file.substr(pos + 1);
            return std::format("#{:<2} {} ({}:{} +0x{:X})", index, sym, file, frame.sourceLine, frame.offset);
        }
        if (!frame.module.empty()) {
            return std::format("#{:<2} {} [{}+0x{:X}]", index, sym, frame.module, frame.offset);
        }
        return std::format("#{:<2} {} (0x{:X})", index, sym, reinterpret_cast<uintptr_t>(frame.address));
    }

    // =========================================================================
    // ToString — plain-text multi-line
    // =========================================================================

    std::string StackTrace::ToString() const {
        std::string result;
        result.reserve(frames_.size() * 120);
        result += std::format("Stack Trace ({} frames):\n", frames_.size());
        for (uint32_t i = 0; i < frames_.size(); ++i) {
            result += "  ";
            result += FormatCompact(frames_[i], i);
            result += '\n';
        }
        return result;
    }

    // =========================================================================
    // PrintColored — tapioca-styled box-drawing output to stderr
    // =========================================================================

    namespace {

        tapioca::basic_console& GetStderrConsole() {
            static std::once_flag initFlag;
            std::call_once(initFlag, [] {
                tapioca::terminal::enable_utf8();
                tapioca::terminal::enable_vt_processing();
            });
            thread_local tapioca::basic_console console{
                tapioca::console_config{.sink = tapioca::pal::stderr_sink()}};
            return console;
        }

        std::string RepeatStr(std::string_view s, uint32_t n) {
            std::string result;
            result.reserve(s.size() * n);
            for (uint32_t i = 0; i < n; ++i) result += s;
            return result;
        }

        using namespace tapioca;

        constexpr style kBox    {.fg = colors::bright_black, .bg = color::default_color(), .attrs = attr::dim};
        constexpr style kIndex  {.fg = colors::white,        .bg = color::default_color(), .attrs = attr::bold};
        constexpr style kSymbol {.fg = colors::bright_cyan,  .bg = color::default_color(), .attrs = attr::bold};
        constexpr style kUnknown{.fg = colors::bright_black, .bg = color::default_color(), .attrs = attr::dim | attr::italic};
        constexpr style kFile   {.fg = colors::green,        .bg = color::default_color(), .attrs = attr::none};
        constexpr style kLine   {.fg = colors::bright_green, .bg = color::default_color(), .attrs = attr::bold};
        constexpr style kOffset {.fg = colors::bright_black, .bg = color::default_color(), .attrs = attr::dim};
        constexpr style kModule {.fg = colors::yellow,       .bg = color::default_color(), .attrs = attr::dim};
        constexpr style kTitle  {.fg = colors::bright_red,   .bg = color::default_color(), .attrs = attr::bold};
        constexpr style kBadge  {.fg = colors::bright_white, .bg = color::default_color(), .attrs = attr::bold | attr::dim};

    } // anonymous namespace

    void StackTrace::PrintColored(std::string_view title) const {
        if (frames_.empty()) return;

        auto& con = GetStderrConsole();
        uint32_t termWidth = con.term_width();
        termWidth = (termWidth < 40) ? 80 : (termWidth > 160 ? 160 : termWidth);
        uint32_t innerWidth = termWidth - 4;

        auto padRight = [&](std::string& buf, uint32_t usedCols) {
            if (usedCols < innerWidth) buf.append(innerWidth - usedCols, ' ');
        };

        std::string buf;
        buf.reserve(4096);

        // Top border: ╭─ Title ──...── N frames ─╮
        {
            std::string titleStr{title};
            std::string countStr = std::format("{} frames", frames_.size());
            uint32_t fixedChars = 6;
            uint32_t contentLen = static_cast<uint32_t>(titleStr.size() + countStr.size()) + 3;
            uint32_t fillLen = (termWidth > fixedChars + contentLen) ? (termWidth - fixedChars - contentLen) : 1;

            con.emit_styled(kBox, "\u256D\u2500 ", buf);
            con.emit_styled(kTitle, titleStr, buf);
            con.emit_styled(kBox, " " + RepeatStr("\u2500", fillLen) + " ", buf);
            con.emit_styled(kBadge, countStr, buf);
            con.emit_styled(kBox, " \u2500\u256E", buf);
            con.emit_reset(buf);
            buf += '\n';
        }

        // Empty line
        auto emitEmpty = [&]() {
            con.emit_styled(kBox, "\u2502", buf);
            buf.append(termWidth - 2, ' ');
            con.emit_styled(kBox, "\u2502", buf);
            con.emit_reset(buf);
            buf += '\n';
        };
        emitEmpty();

        // Frame entries
        for (uint32_t i = 0; i < static_cast<uint32_t>(frames_.size()); ++i) {
            const auto& f = frames_[i];

            // Line 1: │  #N  symbolName                              │
            {
                uint32_t used = 0;
                con.emit_styled(kBox, "\u2502  ", buf); used += 3;

                auto indexStr = std::format("#{:<3}", i);
                con.emit_styled(kIndex, indexStr, buf);
                used += static_cast<uint32_t>(indexStr.size());

                std::string sym = f.symbol.empty() ? "<unknown>" : f.symbol;
                uint32_t maxSym = (innerWidth > used + 2) ? (innerWidth - used - 1) : 20;
                bool trunc = sym.size() > maxSym;
                if (trunc) sym.resize(maxSym - 1);

                con.emit_styled(f.symbol.empty() ? kUnknown : kSymbol, sym, buf);
                if (trunc) con.emit_styled(kBox, "\u2026", buf);
                used += static_cast<uint32_t>(sym.size()) + (trunc ? 1 : 0);

                padRight(buf, used);
                con.emit_styled(kBox, " \u2502", buf);
                con.emit_reset(buf);
                buf += '\n';
            }

            // Line 2: │       @ file:line                     +0xOFF │
            {
                uint32_t used = 0;
                con.emit_styled(kBox, "\u2502      ", buf); used += 7;

                if (!f.sourceFile.empty()) {
                    auto filePath = std::string_view{f.sourceFile};
                    auto pos = filePath.find_last_of("\\/");
                    auto fileName = (pos != std::string_view::npos) ? filePath.substr(pos + 1) : filePath;
                    con.emit_styled(kOffset, "@ ", buf); used += 2;
                    con.emit_styled(kFile, fileName, buf); used += static_cast<uint32_t>(fileName.size());
                    con.emit_styled(kBox, ":", buf); used += 1;
                    auto lineStr = std::to_string(f.sourceLine);
                    con.emit_styled(kLine, lineStr, buf); used += static_cast<uint32_t>(lineStr.size());
                } else if (!f.module.empty()) {
                    con.emit_styled(kOffset, "@ ", buf); used += 2;
                    con.emit_styled(kModule, f.module, buf); used += static_cast<uint32_t>(f.module.size());
                } else {
                    auto addrStr = std::format("@ 0x{:X}", reinterpret_cast<uintptr_t>(f.address));
                    con.emit_styled(kOffset, addrStr, buf); used += static_cast<uint32_t>(addrStr.size());
                }

                auto offsetStr = std::format("+0x{:X}", f.offset);
                uint32_t rightMargin = static_cast<uint32_t>(offsetStr.size()) + 1;
                if (used + rightMargin + 2 < innerWidth) {
                    buf.append(innerWidth - used - rightMargin, ' ');
                    con.emit_styled(kOffset, offsetStr, buf);
                } else {
                    buf += ' ';
                    con.emit_styled(kOffset, offsetStr, buf);
                }
                con.emit_styled(kBox, " \u2502", buf);
                con.emit_reset(buf);
                buf += '\n';
            }

            // Separator (dotted line between frames, skip after last)
            if (i + 1 < static_cast<uint32_t>(frames_.size())) {
                con.emit_styled(kBox, "\u2502  ", buf);
                con.emit_styled(kBox, RepeatStr("\u00B7", innerWidth - 2), buf);
                con.emit_styled(kBox, " \u2502", buf);
                con.emit_reset(buf);
                buf += '\n';
            }
        }

        // Bottom border: ╰─...─╯
        emitEmpty();
        con.emit_styled(kBox, "\u2570" + RepeatStr("\u2500", termWidth - 2) + "\u256F", buf);
        con.emit_reset(buf);
        buf += '\n';

        con.flush_to_sink(buf);
        std::fflush(stderr);
    }

}  // namespace Mashiro
