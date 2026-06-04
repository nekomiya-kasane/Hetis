/**
 * @file StructuredLogger.cpp
 * @brief StructuredLogger implementation: SPSC ring buffer, sinks, drain thread.
 */

#include "Mashiro/Core/StructuredLogger.h"
#include "Mashiro/Core/ToString.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <print>

#include <tapioca/console.h>
#include <tapioca/style.h>

namespace Mashiro {

    // =========================================================================
    // Serialized entry format in ring buffer
    // =========================================================================

    struct SerializedHeader {
        uint8_t level;
        uint16_t category;
        uint32_t line;
        uint64_t timestampNs;
        uint32_t threadId;
        uint16_t fileLen;
        uint16_t funcLen;
        uint16_t msgLen;
    };

    // =========================================================================
    // ConsoleSink (tapioca-based)
    // =========================================================================

    namespace {

        tapioca::style GetLevelStyle(LogLevel level) {
            using namespace tapioca;
            
            constexpr auto buildStyleTable = [] consteval {
                std::array<tapioca::style, 6> table{};
                for (int i = 0; i < 6; ++i) {
                    auto c = Detail::Log::GetLevelColor(static_cast<LogLevel>(i));
                    table[i].fg = color::from_rgb(c.r, c.g, c.b);
                    table[i].bg = color::default_color();
                    table[i].attrs = (c.bold ? attr::bold : attr::none) | (c.dim ? attr::dim : attr::none);
                }
                return table;
            };
            static constexpr auto kStyleTable = buildStyleTable();
            auto idx = static_cast<uint8_t>(level);
            return (idx < kStyleTable.size()) ? kStyleTable[idx] : style{};
        }

        std::string FormatTimestamp(uint64_t ns) {
            using namespace std::chrono;
            auto tp = sys_time<milliseconds>{duration_cast<milliseconds>(nanoseconds{ns})};
            auto dp = floor<days>(tp);
            auto tod = hh_mm_ss{tp - dp};
            return std::format("{:02}:{:02}:{:02}.{:03}",
                tod.hours().count(), tod.minutes().count(),
                tod.seconds().count(), tod.subseconds().count());
        }

        tapioca::basic_console& GetStderrConsole() {
            thread_local tapioca::basic_console console{
                tapioca::console_config{tapioca::pal::stderr_sink()}};
            return console;
        }

    } // anonymous namespace

    ConsoleSink::ConsoleSink() {
        colorEnabled_ = GetStderrConsole().color_enabled();
    }

    void ConsoleSink::Write(const LogEntry& entry) {
        auto& console = GetStderrConsole();
        auto ts = FormatTimestamp(entry.timestampNs);
        auto levelStr = ToStringView(entry.level);
        auto catStr = ToStringView(entry.category);

        std::string line;
        line.reserve(256);
        line += '['; line += ts; line += "] [";
        line += levelStr; line += "] [";
        line += catStr; line += "] ";

        if (!entry.file.empty()) {
            // Extract basename
            auto pos = entry.file.find_last_of("\\/");
            auto fname = (pos != std::string_view::npos) ? entry.file.substr(pos + 1) : entry.file;
            line += fname; line += ':';
            line += std::to_string(entry.line); line += " | ";
        }
        if (!entry.func.empty()) {
            line += entry.func; line += " | ";
        }
        line += entry.message;

        console.styled_write(GetLevelStyle(entry.level), line);
        console.newline();
    }

    void ConsoleSink::Flush() { std::fflush(stderr); }

    // =========================================================================
    // FileSink
    // =========================================================================

    FileSink::FileSink(std::filesystem::path path, bool dailyRotation)
        : basePath_(std::move(path)), rotate_(dailyRotation) {
        file_ = std::fopen(basePath_.string().c_str(), "a");
    }

    FileSink::~FileSink() { if (file_) std::fclose(file_); }

    FileSink::FileSink(FileSink&& o) noexcept
        : file_(o.file_), basePath_(std::move(o.basePath_)), rotate_(o.rotate_) {
        o.file_ = nullptr;
    }

    FileSink& FileSink::operator=(FileSink&& o) noexcept {
        if (this != &o) {
            if (file_) std::fclose(file_);
            file_ = o.file_; basePath_ = std::move(o.basePath_); rotate_ = o.rotate_;
            o.file_ = nullptr;
        }
        return *this;
    }

    void FileSink::Write(const LogEntry& entry) {
        if (!file_) return;
        auto ts = FormatTimestamp(entry.timestampNs);
        std::println(file_, "[{}] [{}] [{}] {} | {}",
            ts, ToStringView(entry.level), ToStringView(entry.category),
            entry.func, entry.message);
    }

    void FileSink::Flush() { if (file_) std::fflush(file_); }

    // =========================================================================
    // JsonSink
    // =========================================================================

    JsonSink::JsonSink(std::filesystem::path path) : path_(std::move(path)) {
        file_ = std::fopen(path_.string().c_str(), "a");
    }

    JsonSink::~JsonSink() { if (file_) std::fclose(file_); }

    JsonSink::JsonSink(JsonSink&& o) noexcept : file_(o.file_), path_(std::move(o.path_)) {
        o.file_ = nullptr;
    }

    JsonSink& JsonSink::operator=(JsonSink&& o) noexcept {
        if (this != &o) {
            if (file_) std::fclose(file_);
            file_ = o.file_; path_ = std::move(o.path_);
            o.file_ = nullptr;
        }
        return *this;
    }

    namespace {
        std::string EscapeJson(std::string_view sv) {
            std::string out;
            out.reserve(sv.size() + 8);
            for (char c : sv) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    else
                        out += c;
                    break;
                }
            }
            return out;
        }
    } // anonymous namespace

    void JsonSink::Write(const LogEntry& entry) {
        if (!file_) return;
        std::println(file_,
            R"({{"ts":{},"level":"{}","cat":"{}","func":"{}","msg":"{}","file":"{}","line":{},"tid":{}}})",
            entry.timestampNs,
            ToStringView(entry.level),
            ToStringView(entry.category),
            EscapeJson(entry.func),
            EscapeJson(entry.message),
            EscapeJson(entry.file),
            entry.line, entry.threadId);
    }

    void JsonSink::Flush() { if (file_) std::fflush(file_); }

    // =========================================================================
    // CallbackSink
    // =========================================================================

    CallbackSink::CallbackSink(Callback cb) : cb_(std::move(cb)) {}
    void CallbackSink::Write(const LogEntry& entry) { if (cb_) cb_(entry); }
    void CallbackSink::Flush() {}

    // =========================================================================
    // StructuredLogger — thread-local ring management
    // =========================================================================

    namespace {
        uint32_t GetCurrentThreadIdHash() {
            return static_cast<uint32_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }
    } // anonymous namespace

    LogRing& StructuredLogger::GetThreadRing() {
        struct RingGuard {
            LogRing ring;
            ~RingGuard() {
                auto& logger = StructuredLogger::Instance();
                std::lock_guard lock(logger.ringsMutex_);
                std::erase_if(logger.rings_,
                    [this](const RegisteredRing& r) { return r.ring == &ring; });
            }
        };

        thread_local RingGuard guard;
        thread_local bool registered = false;

        if (!registered) {
            registered = true;
            auto& logger = Instance();
            std::lock_guard lock(logger.ringsMutex_);
            logger.rings_.push_back({&guard.ring, GetCurrentThreadIdHash()});
        }
        return guard.ring;
    }

    // =========================================================================
    // StructuredLogger — core
    // =========================================================================

    StructuredLogger::StructuredLogger() {
        // Initialize per-category levels from [[=LogAnno::DefaultLevel{N}]] annotations.
        for (std::size_t i = 0; i < categoryLevels_.size(); ++i) {
            uint8_t defaultLvl = (i < Detail::Log::kDefaultCategoryLevels.size())
                ? Detail::Log::kDefaultCategoryLevels[i]
                : 0; // Trace for user-extended categories
            categoryLevels_[i].store(defaultLvl, std::memory_order_relaxed);
        }
    }

    StructuredLogger::~StructuredLogger() { Shutdown(); }

    StructuredLogger& StructuredLogger::Instance() {
        static StructuredLogger instance;
        return instance;
    }

    void StructuredLogger::AddSink(LogSink sink) {
        std::lock_guard lock(sinksMutex_);
        sinks_.push_back(std::move(sink));
    }

    void StructuredLogger::ClearSinks() {
        std::lock_guard lock(sinksMutex_);
        sinks_.clear();
    }

    void StructuredLogger::SetCategoryLevel(LogCategory cat, LogLevel level) {
        auto idx = static_cast<std::size_t>(cat);
        if (idx < categoryLevels_.size())
            categoryLevels_[idx].store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }

    LogLevel StructuredLogger::GetCategoryLevel(LogCategory cat) const {
        auto idx = static_cast<std::size_t>(cat);
        if (idx < categoryLevels_.size())
            return static_cast<LogLevel>(categoryLevels_[idx].load(std::memory_order_relaxed));
        return LogLevel::Trace;
    }

    void StructuredLogger::SetBackpressurePolicy(BackpressurePolicy policy) {
        policy_.store(policy, std::memory_order_relaxed);
    }
    
    BackpressurePolicy StructuredLogger::GetBackpressurePolicy() const {
        return static_cast<BackpressurePolicy>(policy_.load(std::memory_order_relaxed));
    }

    bool StructuredLogger::IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }

    uint64_t StructuredLogger::GetDroppedCount() const {
        return droppedCount_.load(std::memory_order_relaxed);
    }

    void StructuredLogger::ResetDroppedCount() {
        droppedCount_.store(0, std::memory_order_relaxed);
    }

    void StructuredLogger::ResetRings() {
        std::lock_guard lock(ringsMutex_);
        for (auto& reg : rings_)
            if (reg.ring) reg.ring->Reset();
    }

    void StructuredLogger::WriteToRing(
        LogLevel level, LogCategory cat,
        std::string_view file, uint32_t line, std::string_view func,
        uint64_t timestampNs, std::string_view message)
    {
        auto& ring = GetThreadRing();

        SerializedHeader hdr{};
        hdr.level = static_cast<uint8_t>(level);
        hdr.category = static_cast<uint16_t>(cat);
        hdr.line = line;
        hdr.timestampNs = timestampNs;
        hdr.threadId = GetCurrentThreadIdHash();
        hdr.fileLen = static_cast<uint16_t>(std::min<std::size_t>(file.size(), 0xFFFF));
        hdr.funcLen = static_cast<uint16_t>(std::min<std::size_t>(func.size(), 0xFFFF));
        hdr.msgLen = static_cast<uint16_t>(std::min<std::size_t>(message.size(), 0xFFFF));

        uint32_t totalPayload = sizeof(SerializedHeader) + hdr.fileLen + hdr.funcLen + hdr.msgLen;

        thread_local std::vector<std::byte> payload;
        payload.resize(totalPayload);

        auto* dst = payload.data();
        std::memcpy(dst, &hdr, sizeof(hdr)); dst += sizeof(hdr);
        std::memcpy(dst, file.data(), hdr.fileLen); dst += hdr.fileLen;
        std::memcpy(dst, func.data(), hdr.funcLen); dst += hdr.funcLen;
        std::memcpy(dst, message.data(), hdr.msgLen);

        auto payloadSpan = std::span<const std::byte>{payload.data(), totalPayload};
        bool ok = ring.TryWrite(payloadSpan);
        if (!ok) {
            if (policy_.load(std::memory_order_relaxed) == BackpressurePolicy::Block) {
                while (!ring.TryWrite(payloadSpan))
                    std::this_thread::yield();
            } else {
                droppedCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        drainCv_.notify_one();
    }

    void StructuredLogger::StartDrainThread() {
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        drainThread_ = std::thread([this] { DrainLoop(); });
    }

    void StructuredLogger::DrainLoop() {
        while (running_.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock(drainMutex_);
                drainCv_.wait_for(lock, std::chrono::milliseconds(1));
            }
            DrainOnce();

            if (flushRequested_.load(std::memory_order_acquire)) {
                DrainOnce();
                {
                    std::lock_guard lock(sinksMutex_);
                    for (auto& sink : sinks_)
                        std::visit([](auto& s) { s.Flush(); }, sink);
                }
                flushRequested_.store(false, std::memory_order_release);
                {
                    std::lock_guard fLock(flushMutex_);
                    flushDone_.store(true, std::memory_order_release);
                }
                flushCv_.notify_all();
            }
        }

        DrainOnce();
        {
            std::lock_guard lock(sinksMutex_);
            for (auto& sink : sinks_)
                std::visit([](auto& s) { s.Flush(); }, sink);
        }
    }

    uint32_t StructuredLogger::DrainOnce() {
        uint32_t total = 0;
        std::lock_guard lock(ringsMutex_);

        for (auto& reg : rings_) {
            if (!reg.ring) continue;

            total += reg.ring->ReadAll([this](std::span<const std::byte> data) {
                if (data.size() < sizeof(SerializedHeader)) return;

                SerializedHeader hdr{};
                std::memcpy(&hdr, data.data(), sizeof(hdr));

                auto* bytes = reinterpret_cast<const char*>(data.data());
                auto* filePtr = bytes + sizeof(SerializedHeader);
                auto* funcPtr = filePtr + hdr.fileLen;
                auto* msgPtr = funcPtr + hdr.funcLen;

                LogEntry entry{};
                entry.level = static_cast<LogLevel>(hdr.level);
                entry.category = static_cast<LogCategory>(hdr.category);
                entry.file = std::string_view{filePtr, hdr.fileLen};
                entry.func = std::string_view{funcPtr, hdr.funcLen};
                entry.message = std::string_view{msgPtr, hdr.msgLen};
                entry.line = hdr.line;
                entry.timestampNs = hdr.timestampNs;
                entry.threadId = hdr.threadId;

                DispatchToSinks(entry);
            });
        }
        return total;
    }

    void StructuredLogger::DispatchToSinks(const LogEntry& entry) {
        std::lock_guard lock(sinksMutex_);
        for (auto& sink : sinks_)
            std::visit([&entry](auto& s) { s.Write(entry); }, sink);
    }

    void StructuredLogger::Flush() {
        if (!running_.load(std::memory_order_acquire)) {
            DrainOnce();
            std::lock_guard lock(sinksMutex_);
            for (auto& sink : sinks_)
                std::visit([](auto& s) { s.Flush(); }, sink);
            return;
        }

        flushDone_.store(false, std::memory_order_release);
        flushRequested_.store(true, std::memory_order_release);
        drainCv_.notify_one();

        std::unique_lock lock(flushMutex_);
        flushCv_.wait(lock, [this] { return flushDone_.load(std::memory_order_acquire); });
    }

    void StructuredLogger::Shutdown() {
        if (!running_.load(std::memory_order_acquire)) return;
        running_.store(false, std::memory_order_release);
        drainCv_.notify_all();
        if (drainThread_.joinable()) drainThread_.join();
        flushRequested_.store(false, std::memory_order_relaxed);
        flushDone_.store(false, std::memory_order_relaxed);
    }

}  // namespace Mashiro
