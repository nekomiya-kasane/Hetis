/**
 * @file SchedulerTraceTest.cpp
 * @brief Verify scheduler-trace ownership, clock, event, error, and loss-reporting contracts.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/SchedulerTrace.h>
#include <Sora/Platform.h>

#include "PAL/SchedulerTraceDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <type_traits>

using namespace std::chrono_literals;

namespace PAL = Sora::PAL;

namespace {

    template<typename T, size_t Size>
    void WritePayload(std::array<std::byte, Size>& payload, size_t offset, T value) {
        REQUIRE(offset <= payload.size());
        REQUIRE(sizeof(T) <= payload.size() - offset);
        std::memcpy(payload.data() + offset, &value, sizeof(T));
    }

    [[nodiscard]] PAL::NativeSchedulerTraceRecord MakeRecord(
        PAL::NativeSchedulerTraceProvider provider, uint8_t opcode, std::span<const std::byte> payload,
        PAL::NativeSchedulerTracePointerWidth pointerWidth = PAL::NativeSchedulerTracePointerWidth::Bits64) {
        return {
            .provider = provider,
            .opcode = opcode,
            .pointerWidth = pointerWidth,
            .timestampTicks = 123'456,
            .processorIndex = 17,
            .payload = payload,
        };
    }

} // namespace

static_assert(!std::is_copy_constructible_v<Sora::PAL::SchedulerTraceSession>);
static_assert(std::is_nothrow_move_constructible_v<Sora::PAL::SchedulerTraceSession>);
static_assert(std::is_trivially_copyable_v<Sora::PAL::SchedulerEvent>);

TEST_CASE("Scheduler trace rejects invalid queue configuration", "[Sora.PAL.SchedulerTrace]") {
    const auto session = Sora::PAL::SchedulerTraceSession::Start({.eventQueueCapacity = 0});
    REQUIRE_FALSE(session.has_value());
    REQUIRE(session.error() == Sora::ErrorCode::InvalidArgument);
}

TEST_CASE("Empty scheduler trace session rejects reads", "[Sora.PAL.SchedulerTrace]") {
    Sora::PAL::SchedulerTraceSession session;
    std::array<Sora::PAL::SchedulerEvent, 1> events{};
    const auto read = session.Read(events);
    REQUIRE_FALSE(read.has_value());
    REQUIRE(read.error() == Sora::ErrorCode::InvalidState);
}

TEST_CASE("Scheduler trace decoder preserves context-switch and ready-thread fields",
          "[Sora.PAL.SchedulerTrace]") {
    std::array<std::byte, 20> contextSwitch{};
    WritePayload(contextSwitch, 0, uint32_t{0});
    WritePayload(contextSwitch, 4, uint32_t{91});
    WritePayload(contextSwitch, 8, int8_t{13});
    WritePayload(contextSwitch, 9, int8_t{-2});
    WritePayload(contextSwitch, 10, uint8_t{3});
    WritePayload(contextSwitch, 12, uint8_t{5});
    WritePayload(contextSwitch, 13, uint8_t{1});
    WritePayload(contextSwitch, 14, uint8_t{2});
    WritePayload(contextSwitch, 15, int8_t{-1});
    WritePayload(contextSwitch, 16, uint32_t{77});

    const auto switched = PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
        PAL::NativeSchedulerTraceProvider::Thread, PAL::Detail::kContextSwitchOpcode, contextSwitch));
    REQUIRE(switched.has_value());
    REQUIRE(switched->kind == PAL::SchedulerEventKind::ContextSwitch);
    REQUIRE(switched->timestamp.ticks == 123'456);
    REQUIRE(switched->processor.Value() == 17);
    REQUIRE(switched->payload.contextSwitch.oldThread.Value() == 91);
    REQUIRE(switched->payload.contextSwitch.newThread.Value() == 0);
    REQUIRE(switched->payload.contextSwitch.oldPriority == -2);
    REQUIRE(switched->payload.contextSwitch.newPriority == 13);
    REQUIRE(switched->payload.contextSwitch.previousCState == 3);
    REQUIRE(switched->payload.contextSwitch.oldWaitReason == 5);
    REQUIRE(switched->payload.contextSwitch.oldWaitMode == 1);
    REQUIRE(switched->payload.contextSwitch.oldState == 2);
    REQUIRE(switched->payload.contextSwitch.oldWaitIdealProcessor == -1);
    REQUIRE(switched->payload.contextSwitch.newThreadWaitTime == 77);

    std::array<std::byte, 7> readyThread{};
    WritePayload(readyThread, 0, uint32_t{42});
    WritePayload(readyThread, 4, int8_t{-3});
    WritePayload(readyThread, 5, int8_t{4});
    WritePayload(readyThread, 6, uint8_t{0xa5});
    const auto ready = PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
        PAL::NativeSchedulerTraceProvider::Thread, PAL::Detail::kThreadReadyOpcode, readyThread));
    REQUIRE(ready.has_value());
    REQUIRE(ready->kind == PAL::SchedulerEventKind::ThreadReady);
    REQUIRE(ready->payload.threadReady.thread.Value() == 42);
    REQUIRE(ready->payload.threadReady.adjustmentReason == -3);
    REQUIRE(ready->payload.threadReady.priorityIncrement == 4);
    REQUIRE(ready->payload.threadReady.flags == 0xa5);
}

TEST_CASE("Scheduler trace decoder distinguishes lifecycle transitions from rundown snapshots",
          "[Sora.PAL.SchedulerTrace]") {
    std::array<std::byte, 8> threadPayload{};
    WritePayload(threadPayload, 0, uint32_t{10});
    WritePayload(threadPayload, 4, uint32_t{20});

    const auto decodeThread = [&](uint8_t opcode) {
        return PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
            PAL::NativeSchedulerTraceProvider::Thread, opcode, threadPayload));
    };
    const auto threadCreate = decodeThread(PAL::Detail::kLifecycleStartOpcode);
    const auto threadExit = decodeThread(PAL::Detail::kLifecycleEndOpcode);
    REQUIRE(threadCreate.has_value());
    REQUIRE(threadExit.has_value());
    REQUIRE(threadCreate->kind == PAL::SchedulerEventKind::ThreadCreate);
    REQUIRE(threadExit->kind == PAL::SchedulerEventKind::ThreadExit);
    const auto threadSnapshot = decodeThread(PAL::Detail::kLifecycleSnapshotOpcode);
    REQUIRE(threadSnapshot.has_value());
    REQUIRE(threadSnapshot->kind == PAL::SchedulerEventKind::ThreadSnapshot);
    REQUIRE(threadSnapshot->payload.threadLifecycle.process.Value() == 10);
    REQUIRE(threadSnapshot->payload.threadLifecycle.thread.Value() == 20);
    REQUIRE_FALSE(decodeThread(PAL::Detail::kLifecycleSnapshotEndOpcode).has_value());

    SECTION("64-bit process schema") {
        std::array<std::byte, 16> processPayload{};
        WritePayload(processPayload, 0, uint64_t{0x1234'5678'9abc'def0});
        WritePayload(processPayload, 8, uint32_t{30});
        WritePayload(processPayload, 12, uint32_t{10});
        const auto process = PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
            PAL::NativeSchedulerTraceProvider::Process, PAL::Detail::kLifecycleStartOpcode,
            processPayload));
        REQUIRE(process.has_value());
        REQUIRE(process->kind == PAL::SchedulerEventKind::ProcessCreate);
        REQUIRE(process->payload.processLifecycle.uniqueProcessKey == 0x1234'5678'9abc'def0);
        REQUIRE(process->payload.processLifecycle.process.Value() == 30);
        REQUIRE(process->payload.processLifecycle.parent.Value() == 10);
    }

    SECTION("32-bit process schema") {
        std::array<std::byte, 12> processPayload{};
        WritePayload(processPayload, 0, uint32_t{0x89ab'cdef});
        WritePayload(processPayload, 4, uint32_t{30});
        WritePayload(processPayload, 8, uint32_t{10});
        const auto process = PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
            PAL::NativeSchedulerTraceProvider::Process, PAL::Detail::kLifecycleSnapshotOpcode,
            processPayload, PAL::NativeSchedulerTracePointerWidth::Bits32));
        REQUIRE(process.has_value());
        REQUIRE(process->kind == PAL::SchedulerEventKind::ProcessSnapshot);
        REQUIRE(process->payload.processLifecycle.uniqueProcessKey == 0x89ab'cdef);
    }
}

TEST_CASE("Scheduler trace decoder rejects incomplete and unsupported native records",
          "[Sora.PAL.SchedulerTrace]") {
    const std::array<std::byte, 19> truncatedContextSwitch{};
    REQUIRE_FALSE(PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
                      PAL::NativeSchedulerTraceProvider::Thread,
                      PAL::Detail::kContextSwitchOpcode,
                      truncatedContextSwitch))
                      .has_value());

    const std::array<std::byte, 16> payload{};
    REQUIRE_FALSE(PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
                      PAL::NativeSchedulerTraceProvider::Unknown,
                      PAL::Detail::kLifecycleStartOpcode,
                      payload))
                      .has_value());
    REQUIRE_FALSE(PAL::Detail::DecodeSchedulerTraceRecord(MakeRecord(
                      PAL::NativeSchedulerTraceProvider::Process, 0xff, payload))
                      .has_value());
}

TEST_CASE("Native scheduler trace emits normalized timestamped events when available",
          "[Sora.PAL.SchedulerTrace][.privileged]") {
    auto started = Sora::PAL::SchedulerTraceSession::Start({.eventQueueCapacity = 4096});
    if (!started) {
        INFO("trace error=" << static_cast<int>(started.error()));
        if constexpr (Sora::kIsWindows) {
            REQUIRE((started.error() == Sora::ErrorCode::PermissionDenied ||
                     started.error() == Sora::ErrorCode::ResourceBusy ||
                     started.error() == Sora::ErrorCode::NotSupported));
        } else {
            REQUIRE(started.error() == Sora::ErrorCode::NotSupported);
        }
        return;
    }

    Sora::PAL::SchedulerTraceSession session = std::move(*started);
    REQUIRE(session.Clock().ticksPerSecond != 0);
    std::jthread worker{[] {
        for (size_t index = 0; index < 10'000; ++index) {
            std::this_thread::yield();
        }
    }};

    std::array<Sora::PAL::SchedulerEvent, 256> events{};
    size_t observed = 0;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (observed == 0 && std::chrono::steady_clock::now() < deadline) {
        const auto read = session.Read(events, 100ms);
        REQUIRE(read.has_value());
        observed += read->eventCount;
    }
    worker.join();
    REQUIRE(observed != 0);
    REQUIRE(session.Stop().has_value());
}
