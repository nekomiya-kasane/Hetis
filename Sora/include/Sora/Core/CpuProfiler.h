/**
 * @file CpuProfiler.h
 * @brief Low-overhead per-thread CPU scope profiling with Trace Event JSON export.
 * @details
 * Instrument a scope with the RAII type and a C++26 placeholder variable, then export a trace that can be opened by
 * Perfetto or Chromium tracing tools. Multiple declarations named @c _ may appear in one scope:
 * @code{.cpp}
 * Sora::CpuProfiler& profiler = Sora::CpuProfiler::Default();
 * profiler.SetEnabled(true);
 * {
 *     const Sora::CpuProfileScope _{"BuildAccelerationStructure", Sora::LogCategory::Render};
 *     BuildAccelerationStructure();
 * }
 * void RenderFrame() {
 *     const Sora::CpuProfileScope _{Sora::ProfileCurrentFunction};
 *     RenderScene();
 * }
 * profiler.SetEnabled(false);
 * if (auto result = profiler.ExportTrace("cpu-trace.json"); !result) {
 *     HandleTraceError(result.error());
 * }
 * @endcode
 *
 * Event names are non-owning and must have static storage duration. Each producer thread writes to its own fixed-size
 * ring without locks or allocation after first use. Snapshot, clear, and export are cold operations that briefly pause
 * recording, wait for in-flight writes, and then read a coherent view of every registered ring. Registered rings stay
 * allocated until process shutdown, which favors persistent engine worker pools over repeatedly creating short-lived
 * threads.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/LogCategory.h"
#include "Sora/ErrorCode.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <source_location>
#include <string_view>
#include <vector>

namespace Sora {

    /** @brief One completed CPU interval stored by @ref CpuProfiler. */
    struct CpuProfileEvent {
        std::string_view name;                    /**< Static-lifetime event name. */
        LogCategory category = LogCategory::Core; /**< Semantic profiling domain. */
        uint64_t startNanoseconds = 0;            /**< Monotonic start timestamp. */
        uint64_t endNanoseconds = 0;              /**< Monotonic end timestamp. */
        uint64_t threadId = 0;                    /**< Stable process-local thread identifier. */
        uint32_t depth = 0;                       /**< Nesting depth on the producing thread. */
    };

    /** @brief Tag requesting that @ref CpuProfileScope use the call site's function name. */
    struct ProfileCurrentFunctionTag {};

    /** @brief Tag value for profiling the function containing a @ref CpuProfileScope declaration. */
    inline constexpr ProfileCurrentFunctionTag ProfileCurrentFunction{};

    /**
     * @brief Process-local CPU profiler backed by independent per-thread overwrite rings.
     * @note Disabled recording performs one predictable atomic load and does not allocate or lock.
     * @note When a thread ring wraps, its oldest event is overwritten and counted by @ref DroppedEventCount.
     */
    class CpuProfiler {
    public:
        inline static constexpr size_t kThreadBufferCapacity = 4096; /**< Events retained per producer thread. */

        /** @brief Return the process-default profiler. */
        [[nodiscard]] static CpuProfiler& Default() noexcept;

        /** @brief Enable or disable subsequent event recording. */
        void SetEnabled(bool enabled);

        /** @brief Return whether subsequent completed events are eligible for recording. */
        [[nodiscard]] bool IsEnabled() const noexcept;

        /**
         * @brief Record one completed interval on the calling thread.
         * @param[in] name Static-lifetime event name.
         * @param[in] category Semantic profiling domain.
         * @param[in] startNanoseconds Monotonic start timestamp from @ref TimestampNanoseconds.
         * @param[in] endNanoseconds Monotonic end timestamp from @ref TimestampNanoseconds.
         * @param[in] depth Nesting depth on the calling thread.
         */
        void RecordEvent(std::string_view name, LogCategory category, uint64_t startNanoseconds,
                         uint64_t endNanoseconds, uint32_t depth) noexcept;

        /** @brief Return a coherent owning snapshot sorted by start time. */
        [[nodiscard]] std::vector<CpuProfileEvent> SnapshotEvents() const;

        /** @brief Return at most the newest @p maxCount events in chronological order. */
        [[nodiscard]] std::vector<CpuProfileEvent> RecentEvents(size_t maxCount) const;

        /** @brief Return the number of events currently retained across all thread rings. */
        [[nodiscard]] size_t EventCount() const;

        /** @brief Return events lost to ring overwrite or thread-buffer allocation failure since the last clear. */
        [[nodiscard]] uint64_t DroppedEventCount() const;

        /** @brief Discard all retained events and reset overwrite counters while preserving allocated rings. */
        void Clear();

        /**
         * @brief Export a coherent snapshot as Trace Event JSON.
         * @param[in] outputPath Destination path, usually ending in @c .json.
         * @return Success, or @ref ErrorCode::TraceExportFailed when the file cannot be written completely.
         */
        [[nodiscard]] VoidResult ExportTrace(const std::filesystem::path& outputPath) const noexcept;

        /** @brief Return a monotonic high-resolution timestamp in nanoseconds. */
        [[nodiscard]] static uint64_t TimestampNanoseconds() noexcept;

        /** @brief Return the calling thread's current profile nesting depth. */
        [[nodiscard]] static uint32_t CurrentDepth() noexcept;

        /** @brief Return the current depth, then increment it for a nested scope. */
        static uint32_t PushDepth() noexcept;

        /** @brief Decrement the current thread's depth when it is nonzero. */
        static void PopDepth() noexcept;

        ~CpuProfiler();

        CpuProfiler(const CpuProfiler&) = delete;
        CpuProfiler& operator=(const CpuProfiler&) = delete;
        CpuProfiler(CpuProfiler&&) = delete;
        CpuProfiler& operator=(CpuProfiler&&) = delete;

    private:
        CpuProfiler() noexcept;

        struct ThreadBuffer;
        class PauseGuard;

        [[nodiscard]] ThreadBuffer* GetThreadBuffer() noexcept;

        static thread_local uint32_t currentDepth_;
        static thread_local ThreadBuffer* currentThreadBuffer_;

        mutable std::atomic<bool> enabled_{false};
        std::atomic<uint64_t> registrationFailures_{0};
        mutable std::mutex operationMutex_;
        mutable std::mutex registryMutex_;
        std::vector<std::unique_ptr<ThreadBuffer>> buffers_;
    };

    /** @brief RAII interval that records one event when its scope exits. */
    class CpuProfileScope {
    public:
        /**
         * @brief Begin a profiling interval when the default profiler is enabled.
         * @param[in] name Static-lifetime event name.
         * @param[in] category Semantic profiling domain.
         */
        explicit CpuProfileScope(std::string_view name, LogCategory category = LogCategory::Core) noexcept;

        /**
         * @brief Begin an interval named after the function containing the declaration.
         * @param[in] tag Function-name capture tag, normally @ref ProfileCurrentFunction.
         * @param[in] category Semantic profiling domain.
         * @param[in] location Call-site source location captured by the default argument.
         */
        explicit CpuProfileScope(ProfileCurrentFunctionTag tag, LogCategory category = LogCategory::Core,
                                 std::source_location location = std::source_location::current()) noexcept;

        /** @brief Finish and record an active interval. */
        ~CpuProfileScope() noexcept;

        CpuProfileScope(const CpuProfileScope&) = delete;
        CpuProfileScope& operator=(const CpuProfileScope&) = delete;
        CpuProfileScope(CpuProfileScope&&) = delete;
        CpuProfileScope& operator=(CpuProfileScope&&) = delete;

    private:
        std::string_view name_{};
        LogCategory category_ = LogCategory::Core;
        uint64_t startNanoseconds_ = 0;
        uint32_t depth_ = 0;
        bool active_ = false;
    };

} // namespace Sora
