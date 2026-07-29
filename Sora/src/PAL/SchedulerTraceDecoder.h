/**
 * @file SchedulerTraceDecoder.h
 * @brief Declare the private allocation-free decoder for native scheduler trace payloads.
 * @ingroup PAL
 */
#pragma once

#include <Sora/Core/PAL/SchedulerTrace.h>
#include <Sora/Core/PAL/SystemAPI.h>

#include <cstdint>
#include <optional>

namespace Sora::PAL::Detail {

    inline constexpr uint8_t kLifecycleStartOpcode = 1;
    inline constexpr uint8_t kLifecycleEndOpcode = 2;
    inline constexpr uint8_t kLifecycleSnapshotOpcode = 3;
    inline constexpr uint8_t kLifecycleSnapshotEndOpcode = 4;
    inline constexpr uint8_t kContextSwitchOpcode = 36;
    inline constexpr uint8_t kThreadReadyOpcode = 50;

    /** @brief Decode one native record, rejecting unsupported or truncated schemas. */
    [[nodiscard]] std::optional<SchedulerEvent> DecodeSchedulerTraceRecord(
        const NativeSchedulerTraceRecord& record) noexcept;

} // namespace Sora::PAL::Detail
