/**
 * @file SchedulerTrace.h
 * @brief Native scheduler trace session and normalized allocation-free event stream.
 * @ingroup PAL
 *
 * @details Timestamps are raw ticks in the clock described by @ref SchedulerTraceClock. They must not be mixed with
 * @c steady_clock or wall-clock values without an explicit correlation performed by the monitoring layer.
 */
#pragma once

#include <Sora/Core/PAL/NativeIdentity.h>
#include <Sora/ErrorCode.h>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace Sora::PAL {

    /** @brief Strong flat logical-processor index reported by the native trace transport. */
    class LogicalProcessorId {
    public:
        constexpr LogicalProcessorId() noexcept = default;
        explicit constexpr LogicalProcessorId(uint32_t value) noexcept : value_{value} {}

        /** @brief Return the native trace processor index. */
        [[nodiscard]] constexpr uint32_t Value() const noexcept { return value_; }

        friend constexpr auto operator<=>(const LogicalProcessorId&, const LogicalProcessorId&) noexcept = default;

    private:
        uint32_t value_ = 0;
    };

    /** @brief Raw scheduler-trace timestamp in session-clock ticks. */
    struct SchedulerTimestamp {
        int64_t ticks = 0;
        friend constexpr auto operator<=>(const SchedulerTimestamp&, const SchedulerTimestamp&) noexcept = default;
    };

    /** @brief Clock metadata needed to convert raw trace timestamps without changing their epoch. */
    struct SchedulerTraceClock {
        uint64_t ticksPerSecond = 0;
        friend constexpr bool operator==(const SchedulerTraceClock&, const SchedulerTraceClock&) noexcept = default;
    };

    enum class SchedulerEventKind : uint8_t {
        ContextSwitch,
        ThreadReady,
        ThreadCreate,
        ThreadExit,
        ThreadSnapshot,
        ProcessCreate,
        ProcessExit,
        ProcessSnapshot,
    };

    struct ContextSwitchEvent {
        NativeThreadId oldThread{};
        NativeThreadId newThread{};
        int8_t oldPriority = 0;
        int8_t newPriority = 0;
        uint8_t previousCState = 0;
        uint8_t oldState = 0;
        uint8_t oldWaitReason = 0;
        uint8_t oldWaitMode = 0;
        int8_t oldWaitIdealProcessor = 0;
        uint32_t newThreadWaitTime = 0;
    };

    struct ThreadReadyEvent {
        NativeThreadId thread{};
        int8_t priorityIncrement = 0;
        int8_t adjustmentReason = 0;
        uint8_t flags = 0;
    };

    struct ThreadLifecycleEvent {
        NativeProcessId process{};
        NativeThreadId thread{};
    };

    struct ProcessLifecycleEvent {
        uint64_t uniqueProcessKey = 0;
        NativeProcessId process{};
        NativeProcessId parent{};
    };

    union SchedulerEventPayload {
        ContextSwitchEvent contextSwitch;
        ThreadReadyEvent threadReady;
        ThreadLifecycleEvent threadLifecycle;
        ProcessLifecycleEvent processLifecycle;

        constexpr SchedulerEventPayload() noexcept : contextSwitch{} {}
    };

    /**
     * @brief One normalized native scheduler or process/thread lifecycle observation.
     * @details Snapshot kinds report entities present during ETW rundown; they are not creation events. Native identity
     * value zero is preserved because Windows uses it for idle scheduler entities.
     */
    struct SchedulerEvent {
        SchedulerEventKind kind = SchedulerEventKind::ContextSwitch;
        SchedulerTimestamp timestamp{};
        LogicalProcessorId processor{};
        SchedulerEventPayload payload{};
    };

    /** @brief Cumulative loss counters; any nonzero field makes interval statistics incomplete. */
    struct SchedulerTraceLossCounters {
        uint64_t nativeEventsLost = 0;
        uint64_t nativeLogBuffersLost = 0;
        uint64_t nativeRealtimeBuffersLost = 0;
        uint64_t consumerEventsDropped = 0;

        /** @brief Return whether the observed event stream has a known gap. */
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return nativeEventsLost != 0 || nativeLogBuffersLost != 0 || nativeRealtimeBuffersLost != 0 ||
                   consumerEventsDropped != 0;
        }

        friend constexpr bool operator==(const SchedulerTraceLossCounters&,
                                         const SchedulerTraceLossCounters&) noexcept = default;
    };

    struct SchedulerTraceOptions {
        size_t eventQueueCapacity = 65'536;
        uint32_t bufferSizeKilobytes = 64;
        uint32_t minimumBuffers = 64;
        uint32_t maximumBuffers = 256;
    };

    struct SchedulerTraceRead {
        size_t eventCount = 0;
        bool running = false;
        SchedulerTraceLossCounters losses{};
    };

    /** @brief Move-only owner of one native real-time scheduler trace session. */
    class SchedulerTraceSession {
    public:
        SchedulerTraceSession() noexcept;
        SchedulerTraceSession(const SchedulerTraceSession&) = delete;
        SchedulerTraceSession& operator=(const SchedulerTraceSession&) = delete;
        SchedulerTraceSession(SchedulerTraceSession&&) noexcept;
        SchedulerTraceSession& operator=(SchedulerTraceSession&&) noexcept;
        ~SchedulerTraceSession() noexcept;

        /** @brief Acquire Sora's machine-wide scheduler logger and start its real-time consumer. */
        [[nodiscard]] static Result<SchedulerTraceSession> Start(const SchedulerTraceOptions& options = {});

        /** @brief Return the raw timestamp clock used by every event from this session. */
        [[nodiscard]] SchedulerTraceClock Clock() const noexcept;

        /**
         * @brief Read up to @p destination.size events in capture order.
         * @param[out] destination Caller-owned batch storage.
         * @param[in] timeout Maximum wait when no event is currently queued; zero performs a nonblocking read.
         */
        [[nodiscard]] Result<SchedulerTraceRead> Read(std::span<SchedulerEvent> destination,
                                                      std::chrono::milliseconds timeout = {});

        /** @brief Return cumulative loss counters; native buffer totals are final after @ref Stop. */
        [[nodiscard]] SchedulerTraceLossCounters LossCounters() const noexcept;

        /** @brief Stop native collection, drain the consumer, and release the process-global OS resources. */
        [[nodiscard]] VoidResult Stop() noexcept;

        /** @brief Return whether this object currently owns a running trace session. */
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        struct State;
        explicit SchedulerTraceSession(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_{};
    };

    static_assert(std::is_trivially_copyable_v<SchedulerEvent>);

} // namespace Sora::PAL
