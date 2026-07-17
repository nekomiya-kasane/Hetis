/**
 * @file CpuProfiler.cpp
 * @brief Per-thread CPU profiling rings and Trace Event JSON export implementation.
 * @details Use @ref Sora::CpuProfileScope as documented in @ref Sora::CpuProfiler. Recording is lock-free after a
 * thread's first event; snapshot and export are synchronized cold paths.
 * @ingroup Core
 */

#include "Sora/Core/CpuProfiler.h"
#include "Sora/Core/PAL/Thread.h"
#include "Sora/Core/ToJson.h"
#include "Sora/Core/ToString.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <thread>
#include <tuple>
#include <utility>

namespace Sora {

    /**
     * @brief Per-thread buffer for CPU profile events.
     * @details Cache-line aligned ring buffer for lock-free event recording per thread.
     */
    struct alignas(64) CpuProfiler::ThreadBuffer {
        std::atomic<bool> writing{false}; ///< Flag indicating active write operation
        uint64_t writeCount = 0;          ///< Total number of events written
        uint64_t threadId = 0;            ///< Owning thread identifier
        std::string threadName;           ///< Owning thread name
        std::array<CpuProfileEvent, CpuProfiler::kThreadBufferCapacity> events{}; ///< Ring buffer of profile events
    };

    /**
     * @brief RAII guard for pausing profiler during snapshot and export operations.
     * @details Ensures profiling is disabled, all threads finish writing, and holds operation synchronization locks.
     */
    class CpuProfiler::PauseGuard {
    public:
        /**
         * @brief Constructs a pause guard.
         * @param profiler Reference to the profiler to pause.
         * @details Acquires locks, disables profiling, and waits for all thread buffers to finish writing.
         */
        explicit PauseGuard(const CpuProfiler& profiler) noexcept
            : profiler_(profiler),
              operationLock_(profiler.operationMutex_),
              restoreEnabled_(profiler.enabled_.exchange(false, std::memory_order_acq_rel)),
              registryLock_(profiler.registryMutex_) {
            for (const auto& buffer : profiler_.buffers_) {
                while (buffer->writing.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        }

        /**
         * @brief Destroys the pause guard and restores profiler state.
         */
        ~PauseGuard() { profiler_.enabled_.store(restoreEnabled_, std::memory_order_release); }

        PauseGuard(const PauseGuard&) = delete;
        PauseGuard& operator=(const PauseGuard&) = delete;

    private:
        const CpuProfiler& profiler_;                ///< Reference to the profiler
        std::unique_lock<std::mutex> operationLock_; ///< Lock on operation mutex
        bool restoreEnabled_ = false;                ///< Previous profiler enabled state
        std::unique_lock<std::mutex> registryLock_;  ///< Lock on registry mutex
    };

    constinit thread_local uint32_t CpuProfiler::currentDepth_ = 0;
    constinit thread_local CpuProfiler::ThreadBuffer* CpuProfiler::currentThreadBuffer_ = nullptr;

    CpuProfiler& CpuProfiler::Default() noexcept {
        static CpuProfiler profiler;
        return profiler;
    }

    CpuProfiler::CpuProfiler() noexcept = default;

    CpuProfiler::~CpuProfiler() = default;

    void CpuProfiler::SetEnabled(bool enabled) {
        std::lock_guard lock(operationMutex_);
        enabled_.store(enabled, std::memory_order_release);
    }

    bool CpuProfiler::IsEnabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    CpuProfiler::ThreadBuffer* CpuProfiler::GetThreadBuffer() noexcept {
        if (currentThreadBuffer_ != nullptr) {
            return currentThreadBuffer_;
        }

        try {
            // 1. create a buffer for this very thread
            auto buffer = std::make_unique<ThreadBuffer>();
            buffer->threadId = PAL::CurrentNativeThreadId();
            if (auto name = PAL::CurrentThreadName(); name) {
                buffer->threadName = *name;
            }

            // 2. register the buffer in the profiler's registry, which is only owned by the main thread
            ThreadBuffer* registeredBuffer = buffer.get();
            {
                std::lock_guard lock(registryMutex_);
                buffers_.push_back(std::move(buffer));
            }
            currentThreadBuffer_ = registeredBuffer;
            return registeredBuffer;
        } catch (...) {
            registrationFailures_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    void CpuProfiler::RecordEvent(std::string_view name, LogCategory category, uint64_t startNanoseconds,
                                  uint64_t endNanoseconds, uint32_t depth) noexcept {
        if (!enabled_.load(std::memory_order_acquire)) [[likely]] {
            return;
        }
        if (endNanoseconds < startNanoseconds) [[unlikely]] {
            return;
        }

        ThreadBuffer* buffer = GetThreadBuffer();
        if (buffer == nullptr) [[unlikely]] {
            return;
        }

        buffer->writing.store(true, std::memory_order_release);
        if (!enabled_.load(std::memory_order_acquire)) {
            buffer->writing.store(false, std::memory_order_release);
            return;
        }

        const uint64_t sequence = buffer->writeCount;
        buffer->events[sequence % kThreadBufferCapacity] = {.name = name,
                                                            .category = category,
                                                            .startNanoseconds = startNanoseconds,
                                                            .endNanoseconds = endNanoseconds,
                                                            .threadId = buffer->threadId,
                                                            .depth = depth};
        buffer->writeCount = sequence + 1;
        buffer->writing.store(false, std::memory_order_release);
    }

    std::vector<CpuProfileEvent> CpuProfiler::SnapshotEvents() const {
        std::vector<CpuProfileEvent> snapshot;
        {
            PauseGuard pause{*this};
            size_t eventCount = 0;
            for (const auto& buffer : buffers_) {
                eventCount += static_cast<size_t>(std::min<uint64_t>(buffer->writeCount, kThreadBufferCapacity));
            }

            snapshot.reserve(eventCount);
            for (const auto& buffer : buffers_) {
                const uint64_t count = std::min<uint64_t>(buffer->writeCount, kThreadBufferCapacity);
                const uint64_t first = buffer->writeCount - count;
                for (uint64_t sequence = first; sequence < buffer->writeCount; ++sequence) {
                    snapshot.push_back(buffer->events[sequence % kThreadBufferCapacity]);
                }
            }
        }

        std::ranges::sort(snapshot, [](const auto& left, const auto& right) {
            return std::tie(left.startNanoseconds, left.threadId, left.depth) <
                   std::tie(right.startNanoseconds, right.threadId, right.depth);
        });
        return snapshot;
    }

    std::vector<CpuProfileEvent> CpuProfiler::RecentEvents(size_t maxCount) const {
        std::vector<CpuProfileEvent> events = SnapshotEvents();
        if (events.size() > maxCount) {
            events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(maxCount));
        }
        return events;
    }

    size_t CpuProfiler::EventCount() const {
        PauseGuard pause{*this};
        size_t count = 0;
        for (const auto& buffer : buffers_) {
            count += static_cast<size_t>(std::min<uint64_t>(buffer->writeCount, kThreadBufferCapacity));
        }
        return count;
    }

    uint64_t CpuProfiler::DroppedEventCount() const {
        PauseGuard pause{*this};
        uint64_t count = registrationFailures_.load(std::memory_order_relaxed);
        for (const auto& buffer : buffers_) {
            if (buffer->writeCount > kThreadBufferCapacity) {
                count += buffer->writeCount - kThreadBufferCapacity;
            }
        }
        return count;
    }

    void CpuProfiler::Clear() {
        PauseGuard pause{*this};
        for (const auto& buffer : buffers_) {
            buffer->writeCount = 0;
        }
        registrationFailures_.store(0, std::memory_order_relaxed);
    }

    VoidResult CpuProfiler::ExportTrace(const std::filesystem::path& outputPath) const noexcept {
        try {
            const std::vector<CpuProfileEvent> events = SnapshotEvents();
            std::ofstream output{outputPath, std::ios::binary | std::ios::trunc};
            if (!output) {
                return std::unexpected(ErrorCode::TraceExportFailed);
            }

            Json traceEvents = Json::array();
            for (const CpuProfileEvent& event : events) {
                traceEvents.push_back(ToJson(event));
            }

            const Json trace = Json::object({
                {"traceEvents", std::move(traceEvents)},
                {"displayTimeUnit", "ns"},
            });
            output << trace.dump() << '\n';
            output.close();
            return output ? VoidResult{} : VoidResult{std::unexpected(ErrorCode::TraceExportFailed)};
        } catch (...) {
            return std::unexpected(ErrorCode::TraceExportFailed);
        }
    }

    uint64_t CpuProfiler::TimestampNanoseconds() noexcept {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp).count());
    }

    uint32_t CpuProfiler::CurrentDepth() noexcept {
        return currentDepth_;
    }

    uint32_t CpuProfiler::PushDepth() noexcept {
        return currentDepth_++;
    }

    void CpuProfiler::PopDepth() noexcept {
        if (currentDepth_ != 0) {
            --currentDepth_;
        }
    }

    CpuProfileScope::CpuProfileScope(std::string_view name, LogCategory category) noexcept
        : name_(name), category_(category), active_(CpuProfiler::Default().IsEnabled()) {
        if (active_) {
            depth_ = CpuProfiler::PushDepth();
            startNanoseconds_ = CpuProfiler::TimestampNanoseconds();
        }
    }

    CpuProfileScope::CpuProfileScope(ProfileCurrentFunctionTag, LogCategory category,
                                     std::source_location location) noexcept
        : CpuProfileScope(location.function_name(), category) {}

    CpuProfileScope::~CpuProfileScope() noexcept {
        if (!active_) {
            return;
        }
        const uint64_t endNanoseconds = CpuProfiler::TimestampNanoseconds();
        CpuProfiler::PopDepth();
        CpuProfiler::Default().RecordEvent(name_, category_, startNanoseconds_, endNanoseconds, depth_);
    }

} // namespace Sora
