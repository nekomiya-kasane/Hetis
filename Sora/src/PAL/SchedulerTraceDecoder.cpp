/**
 * @file SchedulerTraceDecoder.cpp
 * @brief Decode native scheduler trace payloads into stable PAL events.
 * @ingroup PAL
 */

#include "SchedulerTraceDecoder.h"

#include <cstring>
#include <memory>
#include <optional>

namespace Sora::PAL::Detail {

    namespace {

        template<typename T>
        [[nodiscard]] bool ReadPayload(std::span<const std::byte> payload, size_t offset, T& value) noexcept {
            if (offset > payload.size() || sizeof(T) > payload.size() - offset) {
                return false;
            }
            std::memcpy(&value, payload.data() + offset, sizeof(T));
            return true;
        }

        [[nodiscard]] std::optional<SchedulerEventKind> ThreadLifecycleKind(uint8_t opcode) noexcept {
            switch (opcode) {
                case kLifecycleStartOpcode:
                    return SchedulerEventKind::ThreadCreate;
                case kLifecycleEndOpcode:
                    return SchedulerEventKind::ThreadExit;
                case kLifecycleSnapshotOpcode:
                    return SchedulerEventKind::ThreadSnapshot;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<SchedulerEventKind> ProcessLifecycleKind(uint8_t opcode) noexcept {
            switch (opcode) {
                case kLifecycleStartOpcode:
                    return SchedulerEventKind::ProcessCreate;
                case kLifecycleEndOpcode:
                    return SchedulerEventKind::ProcessExit;
                case kLifecycleSnapshotOpcode:
                    return SchedulerEventKind::ProcessSnapshot;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<SchedulerEvent> DecodeContextSwitch(
            const NativeSchedulerTraceRecord& record) noexcept {
            ContextSwitchEvent payload{};
            uint32_t oldThread = 0;
            uint32_t newThread = 0;
            if (!ReadPayload(record.payload, 0, newThread) || !ReadPayload(record.payload, 4, oldThread) ||
                !ReadPayload(record.payload, 8, payload.newPriority) ||
                !ReadPayload(record.payload, 9, payload.oldPriority) ||
                !ReadPayload(record.payload, 10, payload.previousCState) ||
                !ReadPayload(record.payload, 12, payload.oldWaitReason) ||
                !ReadPayload(record.payload, 13, payload.oldWaitMode) ||
                !ReadPayload(record.payload, 14, payload.oldState) ||
                !ReadPayload(record.payload, 15, payload.oldWaitIdealProcessor) ||
                !ReadPayload(record.payload, 16, payload.newThreadWaitTime)) {
                return std::nullopt;
            }
            payload.oldThread = NativeThreadId{oldThread};
            payload.newThread = NativeThreadId{newThread};

            SchedulerEvent event{};
            event.kind = SchedulerEventKind::ContextSwitch;
            event.timestamp = SchedulerTimestamp{record.timestampTicks};
            event.processor = LogicalProcessorId{record.processorIndex};
            event.payload.contextSwitch = payload;
            return event;
        }

        [[nodiscard]] std::optional<SchedulerEvent> DecodeThreadReady(
            const NativeSchedulerTraceRecord& record) noexcept {
            ThreadReadyEvent payload{};
            uint32_t thread = 0;
            if (!ReadPayload(record.payload, 0, thread) ||
                !ReadPayload(record.payload, 4, payload.adjustmentReason) ||
                !ReadPayload(record.payload, 5, payload.priorityIncrement) ||
                !ReadPayload(record.payload, 6, payload.flags)) {
                return std::nullopt;
            }
            payload.thread = NativeThreadId{thread};

            SchedulerEvent event{};
            event.kind = SchedulerEventKind::ThreadReady;
            event.timestamp = SchedulerTimestamp{record.timestampTicks};
            event.processor = LogicalProcessorId{record.processorIndex};
            std::construct_at(&event.payload.threadReady, payload);
            return event;
        }

        [[nodiscard]] std::optional<SchedulerEvent> DecodeThreadLifecycle(
            const NativeSchedulerTraceRecord& record) noexcept {
            const std::optional kind = ThreadLifecycleKind(record.opcode);
            if (!kind) {
                return std::nullopt;
            }
            uint32_t process = 0;
            uint32_t thread = 0;
            if (!ReadPayload(record.payload, 0, process) || !ReadPayload(record.payload, 4, thread)) {
                return std::nullopt;
            }

            SchedulerEvent event{};
            event.kind = *kind;
            event.timestamp = SchedulerTimestamp{record.timestampTicks};
            event.processor = LogicalProcessorId{record.processorIndex};
            std::construct_at(&event.payload.threadLifecycle, ThreadLifecycleEvent{
                .process = NativeProcessId{process},
                .thread = NativeThreadId{thread},
            });
            return event;
        }

        [[nodiscard]] std::optional<SchedulerEvent> DecodeProcessLifecycle(
            const NativeSchedulerTraceRecord& record) noexcept {
            const std::optional kind = ProcessLifecycleKind(record.opcode);
            if (!kind) {
                return std::nullopt;
            }
            const size_t pointerSize = static_cast<size_t>(record.pointerWidth);
            uint64_t uniqueProcessKey = 0;
            uint32_t process = 0;
            uint32_t parent = 0;
            if (pointerSize == sizeof(uint32_t)) {
                uint32_t narrowKey = 0;
                if (!ReadPayload(record.payload, 0, narrowKey)) {
                    return std::nullopt;
                }
                uniqueProcessKey = narrowKey;
            } else if (pointerSize == sizeof(uint64_t)) {
                if (!ReadPayload(record.payload, 0, uniqueProcessKey)) {
                    return std::nullopt;
                }
            } else {
                return std::nullopt;
            }
            if (!ReadPayload(record.payload, pointerSize, process) ||
                !ReadPayload(record.payload, pointerSize + sizeof(uint32_t), parent)) {
                return std::nullopt;
            }

            SchedulerEvent event{};
            event.kind = *kind;
            event.timestamp = SchedulerTimestamp{record.timestampTicks};
            event.processor = LogicalProcessorId{record.processorIndex};
            std::construct_at(&event.payload.processLifecycle, ProcessLifecycleEvent{
                .uniqueProcessKey = uniqueProcessKey,
                .process = NativeProcessId{process},
                .parent = NativeProcessId{parent},
            });
            return event;
        }

    } // namespace

    std::optional<SchedulerEvent> DecodeSchedulerTraceRecord(const NativeSchedulerTraceRecord& record) noexcept {
        switch (record.provider) {
            case NativeSchedulerTraceProvider::Thread:
                if (record.opcode == kContextSwitchOpcode) {
                    return DecodeContextSwitch(record);
                }
                if (record.opcode == kThreadReadyOpcode) {
                    return DecodeThreadReady(record);
                }
                return DecodeThreadLifecycle(record);
            case NativeSchedulerTraceProvider::Process:
                return DecodeProcessLifecycle(record);
            case NativeSchedulerTraceProvider::Unknown:
                return std::nullopt;
        }
    }

} // namespace Sora::PAL::Detail
