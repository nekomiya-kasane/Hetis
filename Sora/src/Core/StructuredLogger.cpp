/**
 * @file StructuredLogger.cpp
 * @brief Structured logger sinks, filtering, and dispatch implementation.
 * @ingroup Core
 */

#include <Sora/Core/StructuredLogger.h>
#include <Sora/Core/Path.h>
#include <Sora/Core/PAL/Thread.h>
#include <Sora/Core/ToJson.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <span>

namespace Sora {

    namespace {

        /** @brief Return the built-in default threshold for @p category. */
        [[nodiscard]] constexpr LogLevel DefaultCategoryLevel(LogCategory category) noexcept {
            switch (category) {
                case LogCategory::Core:
                case LogCategory::Kernel:
                case LogCategory::Resource:
                case LogCategory::Render:
                case LogCategory::Audio:
                case LogCategory::Network:
                    return LogLevel::Info;
                case LogCategory::Scene:
                case LogCategory::Input:
                case LogCategory::Script:
                case LogCategory::Editor:
                    return LogLevel::Debug;
                case LogCategory::App:
                    return LogLevel::Trace;
            }
            return LogLevel::Trace;
        }

        /** @brief Format @p time as local wall-clock time with millisecond precision. */
        [[nodiscard]] std::string FormatTimestamp(std::chrono::system_clock::time_point time) {
            using namespace std::chrono;
            const auto seconds = time_point_cast<std::chrono::seconds>(time);
            const auto millis = duration_cast<milliseconds>(time - seconds).count();
            const std::time_t tt = system_clock::to_time_t(time);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", tm.tm_year + 1900, tm.tm_mon + 1,
                               tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
        }

        /** @brief Build one plain log line. */
        [[nodiscard]] std::string FormatPlainRecord(const LogRecord& record) {
            return std::format("[{}] [{}] [{}] [{}] {}:{} | {}", FormatTimestamp(record.timestamp),
                               Traits::EnumToString(record.level), Traits::EnumToString(record.category),
                               record.threadId, record.source.FileName(), record.source.line, record.message);
        }

        /** @brief Build one styled log line through Sora's terminal-safe rich string builder. */
        [[nodiscard]] std::string FormatStyledRecord(const LogRecord& record) {
            namespace Styled = Sora::Styled;
            Styled::StyledStringBuilder builder{};
            const Styled::StyledRole levelRole =
                record.level >= LogLevel::Error ? Styled::StyledRole::Error : Styled::StyledRole::EnumName;

            builder.Raw(Styled::StyledRole::Punctuation, "[");
            builder.Text(Styled::StyledRole::Number, FormatTimestamp(record.timestamp));
            builder.Raw(Styled::StyledRole::Punctuation, "] [");
            builder.Text(levelRole, Traits::EnumToString(record.level));
            builder.Raw(Styled::StyledRole::Punctuation, "] [");
            builder.Text(Styled::StyledRole::FieldName, Traits::EnumToString(record.category));
            builder.Raw(Styled::StyledRole::Punctuation, "] [");
            builder.Text(Styled::StyledRole::Address, std::format("{}", record.threadId));
            builder.Raw(Styled::StyledRole::Punctuation, "] ");
            builder.Text(Styled::StyledRole::String, record.source.FileName());
            builder.Raw(Styled::StyledRole::Punctuation, ":");
            builder.Text(Styled::StyledRole::Number, std::format("{}", record.source.line));
            builder.Raw(Styled::StyledRole::Plain, " | ");
            builder.Text(Styled::StyledRole::Plain, record.message);
            return std::move(builder).Finish();
        }

        /** @brief Close a C file handle and null it. */
        void CloseFile(std::FILE*& file) noexcept {
            if (file != nullptr) {
                std::fclose(file);
                file = nullptr;
            }
        }

        /** @brief Stable NDJSON wire view for @ref JsonLogSink. */
        struct JsonLogRecord {
            int64_t ts = 0;
            std::string level;
            std::string category;
            uint64_t thread = 0;
            std::string_view file;
            uint32_t line = 0;
            std::string_view function;
            std::string_view message;
        };

    } // namespace

    /** @brief Serialized record header stored in a thread-local byte ring. */
    struct SerializedLogHeader {
        uint8_t level = 0;
        uint16_t category = 0;
        uint32_t line = 0;
        uint32_t column = 0;
        uint64_t timestampNs = 0;
        uint64_t threadId = 0;
        uint32_t fileSize = 0;
        uint32_t functionSize = 0;
        uint32_t messageSize = 0;
    };

    /** @brief Single-producer/single-consumer byte ring used by one producer thread and the logger drain thread. */
    struct StructuredLogger::ThreadRing {
        static constexpr size_t kCapacity = size_t{64} * 1024;

        [[nodiscard]] bool TryWrite(std::span<const std::byte> payload) noexcept {
            const size_t totalSize = sizeof(uint32_t) + payload.size_bytes();
            if (totalSize > kCapacity) {
                return false;
            }

            const size_t write = writeIndex_.load(std::memory_order_relaxed);
            const size_t read = readIndex_.load(std::memory_order_acquire);
            if (kCapacity - (write - read) < totalSize) {
                return false;
            }

            const uint32_t payloadSize = static_cast<uint32_t>(payload.size_bytes());
            WriteBytes(write, std::as_bytes(std::span{&payloadSize, 1}));
            WriteBytes(write + sizeof(payloadSize), payload);
            writeIndex_.store(write + totalSize, std::memory_order_release);
            return true;
        }

        template<typename Fn>
        uint32_t ReadAll(Fn&& fn) {
            uint32_t count = 0;
            size_t read = readIndex_.load(std::memory_order_relaxed);
            const size_t write = writeIndex_.load(std::memory_order_acquire);

            while (read + sizeof(uint32_t) <= write) {
                uint32_t payloadSize = 0;
                ReadBytes(read, std::as_writable_bytes(std::span{&payloadSize, 1}));
                if (read + sizeof(uint32_t) + payloadSize > write) {
                    break;
                }

                scratch_.resize(payloadSize);
                ReadBytes(read + sizeof(uint32_t), std::span<std::byte>{scratch_.data(), scratch_.size()});
                fn(std::span<const std::byte>{scratch_.data(), scratch_.size()});
                read += sizeof(uint32_t) + payloadSize;
                ++count;
            }

            readIndex_.store(read, std::memory_order_release);
            return count;
        }

    private:
        void WriteBytes(size_t index, std::span<const std::byte> bytes) noexcept {
            const size_t offset = index % kCapacity;
            const size_t first = std::min(bytes.size_bytes(), kCapacity - offset);
            std::memcpy(storage_.data() + offset, bytes.data(), first);
            if (first < bytes.size_bytes()) {
                std::memcpy(storage_.data(), bytes.data() + first, bytes.size_bytes() - first);
            }
        }

        void ReadBytes(size_t index, std::span<std::byte> bytes) noexcept {
            const size_t offset = index % kCapacity;
            const size_t first = std::min(bytes.size_bytes(), kCapacity - offset);
            std::memcpy(bytes.data(), storage_.data() + offset, first);
            if (first < bytes.size_bytes()) {
                std::memcpy(bytes.data() + first, storage_.data(), bytes.size_bytes() - first);
            }
        }

        alignas(64) std::atomic<size_t> readIndex_{0};
        alignas(64) std::atomic<size_t> writeIndex_{0};
        alignas(64) std::array<std::byte, kCapacity> storage_{};
        std::vector<std::byte> scratch_{};
    };

    std::string_view SourceLocation::FileName() const noexcept {
        return Sora::FileName(file);
    }

    void ConsoleLogSink::Write(const LogRecord& record) {
        const std::string line = styled_ ? FormatStyledRecord(record) : FormatPlainRecord(record);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    }

    void ConsoleLogSink::Flush() noexcept {
        std::fflush(stderr);
    }

    FileLogSink::FileLogSink(std::filesystem::path path) : path_(std::move(path)) {
        file_ = std::fopen(path_.string().c_str(), "a");
    }

    FileLogSink::~FileLogSink() {
        CloseFile(file_);
    }

    FileLogSink::FileLogSink(FileLogSink&& other) noexcept : path_(std::move(other.path_)), file_(other.file_) {
        other.file_ = nullptr;
    }

    FileLogSink& FileLogSink::operator=(FileLogSink&& other) noexcept {
        if (this != &other) {
            CloseFile(file_);
            path_ = std::move(other.path_);
            file_ = other.file_;
            other.file_ = nullptr;
        }
        return *this;
    }

    void FileLogSink::Write(const LogRecord& record) {
        if (file_ == nullptr) {
            return;
        }
        const std::string line = FormatPlainRecord(record);
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fputc('\n', file_);
    }

    void FileLogSink::Flush() noexcept {
        if (file_ != nullptr) {
            std::fflush(file_);
        }
    }

    JsonLogSink::JsonLogSink(std::filesystem::path path) : path_(std::move(path)) {
        file_ = std::fopen(path_.string().c_str(), "a");
    }

    JsonLogSink::~JsonLogSink() {
        CloseFile(file_);
    }

    JsonLogSink::JsonLogSink(JsonLogSink&& other) noexcept : path_(std::move(other.path_)), file_(other.file_) {
        other.file_ = nullptr;
    }

    JsonLogSink& JsonLogSink::operator=(JsonLogSink&& other) noexcept {
        if (this != &other) {
            CloseFile(file_);
            path_ = std::move(other.path_);
            file_ = other.file_;
            other.file_ = nullptr;
        }
        return *this;
    }

    void JsonLogSink::Write(const LogRecord& record) {
        if (file_ == nullptr) {
            return;
        }
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch());
        const JsonLogRecord view{.ts = millis.count(),
                                 .level = Traits::EnumToString(record.level),
                                 .category = Traits::EnumToString(record.category),
                                 .thread = record.threadId,
                                 .file = record.source.file,
                                 .line = record.source.line,
                                 .function = record.source.function,
                                 .message = record.message};
        const std::string line = Sora::ToJson(view).dump();
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fputc('\n', file_);
    }

    void JsonLogSink::Flush() noexcept {
        if (file_ != nullptr) {
            std::fflush(file_);
        }
    }

    void CallbackLogSink::Write(const LogRecord& record) {
        if (callback_) {
            callback_(record);
        }
    }

    StructuredLogger& StructuredLogger::Default() noexcept {
        static StructuredLogger logger;
        return logger;
    }

    StructuredLogger::StructuredLogger() {
        for (size_t i = 0; i < categoryLevels_.size(); ++i) {
            const auto category = static_cast<LogCategory>(i);
            categoryLevels_[i].store(static_cast<uint8_t>(DefaultCategoryLevel(category)), std::memory_order_relaxed);
        }
        running_.store(true, std::memory_order_release);
        drainThread_ = std::thread([this] { DrainLoop(); });
    }

    StructuredLogger::~StructuredLogger() {
        Shutdown();
    }

    void StructuredLogger::AddSink(LogSink sink) {
        std::lock_guard lock(mutex_);
        sinks_.push_back(std::move(sink));
    }

    void StructuredLogger::ClearSinks() {
        std::lock_guard lock(mutex_);
        sinks_.clear();
    }

    void StructuredLogger::SetCategoryLevel(LogCategory category, LogLevel level) noexcept {
        const size_t index = static_cast<size_t>(category);
        if (index < categoryLevels_.size()) {
            categoryLevels_[index].store(static_cast<uint8_t>(level), std::memory_order_relaxed);
        }
    }

    LogLevel StructuredLogger::GetCategoryLevel(LogCategory category) const noexcept {
        const size_t index = static_cast<size_t>(category);
        if (index >= categoryLevels_.size()) {
            return LogLevel::Trace;
        }
        return static_cast<LogLevel>(categoryLevels_[index].load(std::memory_order_relaxed));
    }

    bool StructuredLogger::ShouldLog(LogLevel level, LogCategory category) const noexcept {
        if (static_cast<uint8_t>(level) < static_cast<uint8_t>(kMinLogLevel)) {
            return false;
        }
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(GetCategoryLevel(category));
    }

    void StructuredLogger::SetBackpressurePolicy(LogBackpressurePolicy policy) noexcept {
        backpressurePolicy_.store(policy, std::memory_order_relaxed);
    }

    LogBackpressurePolicy StructuredLogger::GetBackpressurePolicy() const noexcept {
        return backpressurePolicy_.load(std::memory_order_relaxed);
    }

    uint64_t StructuredLogger::DroppedCount() const noexcept {
        return droppedCount_.load(std::memory_order_relaxed);
    }

    void StructuredLogger::ResetDroppedCount() noexcept {
        droppedCount_.store(0, std::memory_order_relaxed);
    }

    void StructuredLogger::Submit(LogLevel level, LogCategory category, SourceLocation source, std::string message) {
        if (!ShouldLog(level, category)) {
            return;
        }

        auto threadId = PAL::CurrentNativeThreadId();
        LogRecord record{.level = level,
                         .category = category,
                         .source = source,
                         .timestamp = std::chrono::system_clock::now(),
                         .threadId = threadId ? *threadId : 0,
                         .message = std::move(message)};
        WriteToRing(record);
    }

    void StructuredLogger::WriteToRing(const LogRecord& record) {
        const auto timestampNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(record.timestamp.time_since_epoch()).count();
        const SerializedLogHeader header{.level = static_cast<uint8_t>(record.level),
                                         .category = static_cast<uint16_t>(record.category),
                                         .line = record.source.line,
                                         .column = record.source.column,
                                         .timestampNs = static_cast<uint64_t>(timestampNs),
                                         .threadId = record.threadId,
                                         .fileSize = static_cast<uint32_t>(record.source.file.size()),
                                         .functionSize = static_cast<uint32_t>(record.source.function.size()),
                                         .messageSize = static_cast<uint32_t>(record.message.size())};
        const size_t payloadSize = sizeof(header) + header.fileSize + header.functionSize + header.messageSize;
        thread_local std::vector<std::byte> payload;
        payload.resize(payloadSize);

        std::byte* cursor = payload.data();
        std::memcpy(cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(cursor, record.source.file.data(), header.fileSize);
        cursor += header.fileSize;
        std::memcpy(cursor, record.source.function.data(), header.functionSize);
        cursor += header.functionSize;
        std::memcpy(cursor, record.message.data(), header.messageSize);

        ThreadRing& ring = GetThreadRing();
        const std::span<const std::byte> bytes{payload.data(), payload.size()};
        if (ring.TryWrite(bytes)) {
            drainCv_.notify_one();
            return;
        }

        if (backpressurePolicy_.load(std::memory_order_relaxed) == LogBackpressurePolicy::Block) {
            while (!ring.TryWrite(bytes)) {
                std::this_thread::yield();
            }
            drainCv_.notify_one();
        } else {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void StructuredLogger::DispatchSerialized(std::span<const std::byte> payload) {
        if (payload.size_bytes() < sizeof(SerializedLogHeader)) {
            return;
        }

        SerializedLogHeader header{};
        std::memcpy(&header, payload.data(), sizeof(header));
        const size_t expectedSize = sizeof(header) + header.fileSize + header.functionSize + header.messageSize;
        if (payload.size_bytes() != expectedSize) {
            return;
        }

        const char* cursor = reinterpret_cast<const char*>(payload.data() + sizeof(header));
        const std::string_view file{cursor, header.fileSize};
        cursor += header.fileSize;
        const std::string_view function{cursor, header.functionSize};
        cursor += header.functionSize;
        std::string message{cursor, header.messageSize};

        LogRecord record{
            .level = static_cast<LogLevel>(header.level),
            .category = static_cast<LogCategory>(header.category),
            .source = {.file = file, .function = function, .line = header.line, .column = header.column},
            .timestamp =
                std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::nanoseconds{static_cast<int64_t>(header.timestampNs)})},
            .threadId = header.threadId,
            .message = std::move(message)};
        DispatchToSinks(record);
    }

    void StructuredLogger::DispatchToSinks(const LogRecord& record) {
        std::lock_guard lock(mutex_);
        for (LogSink& sink : sinks_) {
            std::visit([&record](auto& concrete) { concrete.Write(record); }, sink);
        }
    }

    uint32_t StructuredLogger::DrainOnce() {
        uint32_t total = 0;
        std::lock_guard lock(ringsMutex_);
        for (RegisteredRing& registration : rings_) {
            if (registration.ring != nullptr) {
                total += registration.ring->ReadAll(
                    [this](std::span<const std::byte> payload) { DispatchSerialized(payload); });
            }
        }
        return total;
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
                    std::lock_guard lock(mutex_);
                    for (LogSink& sink : sinks_) {
                        std::visit([](auto& concrete) { concrete.Flush(); }, sink);
                    }
                }
                flushRequested_.store(false, std::memory_order_release);
                {
                    std::lock_guard lock(flushMutex_);
                    flushDone_.store(true, std::memory_order_release);
                }
                flushCv_.notify_all();
            }
        }

        DrainOnce();
        std::lock_guard lock(mutex_);
        for (LogSink& sink : sinks_) {
            std::visit([](auto& concrete) { concrete.Flush(); }, sink);
        }
    }

    StructuredLogger::ThreadRing& StructuredLogger::GetThreadRing() {
        struct RingGuard {
            ThreadRing ring{};

            ~RingGuard() noexcept {
                try {
                    StructuredLogger& logger = StructuredLogger::Default();
                    std::lock_guard lock(logger.ringsMutex_);
                    std::erase_if(logger.rings_,
                                  [this](const RegisteredRing& registration) { return registration.ring == &ring; });
                    try {
                        ring.ReadAll(
                            [&logger](std::span<const std::byte> payload) { logger.DispatchSerialized(payload); });
                    } catch (...) {
                        logger.droppedCount_.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    return;
                }
            }
        };

        thread_local RingGuard guard;
        thread_local bool registered = false;
        if (!registered) {
            registered = true;
            StructuredLogger& logger = StructuredLogger::Default();
            std::lock_guard lock(logger.ringsMutex_);
            auto threadId = PAL::CurrentNativeThreadId();
            logger.rings_.push_back({.ring = &guard.ring, .threadId = threadId ? *threadId : 0});
        }
        return guard.ring;
    }

    void StructuredLogger::Flush() {
        if (!running_.load(std::memory_order_acquire)) {
            DrainOnce();
            std::lock_guard lock(mutex_);
            for (LogSink& sink : sinks_) {
                std::visit([](auto& concrete) { concrete.Flush(); }, sink);
            }
            return;
        }

        flushDone_.store(false, std::memory_order_release);
        flushRequested_.store(true, std::memory_order_release);
        drainCv_.notify_one();

        std::unique_lock lock(flushMutex_);
        flushCv_.wait(lock, [this] { return flushDone_.load(std::memory_order_acquire); });
    }

    void StructuredLogger::Shutdown() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        drainCv_.notify_all();
        if (drainThread_.joinable()) {
            drainThread_.join();
        }
        flushRequested_.store(false, std::memory_order_relaxed);
        flushDone_.store(false, std::memory_order_relaxed);
    }

} // namespace Sora
