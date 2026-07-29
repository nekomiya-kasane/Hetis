/**
 * @file Process.h
 * @brief Current-process identity, startup metadata, and resource-usage introspection.
 * @details
 * This surface observes the calling process without acquiring a controllable process handle. Stable startup metadata
 * and dynamic resource usage remain separate so callers that only need an identifier do not pay for path conversion,
 * argument materialization, or native accounting queries:
 * @code{.cpp}
 * const auto process = Sora::PAL::CurrentNativeProcessId();
 * const auto image = Sora::PAL::CurrentProcessImagePath();
 * const auto usage = Sora::PAL::CaptureCurrentProcessUsage();
 * @endcode
 *
 * Process creation, waiting, termination, inter-process access, and arbitrary-process inspection require native-handle
 * ownership and permission semantics and therefore do not belong to this introspection API. The process environment is
 * modeled independently by @ref Environment.h; the working directory remains a @c std::filesystem facility.
 * @ingroup PAL
 */
#pragma once

#include <Sora/Core/PAL/NativeIdentity.h>
#include <Sora/ErrorCode.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Sora::PAL {

    /** @brief One non-atomic observation of the calling process's accumulated CPU time and resident memory. */
    struct ProcessUsage {
        std::chrono::nanoseconds userCpuTime{};   /**< CPU time spent executing application code. */
        std::chrono::nanoseconds kernelCpuTime{}; /**< CPU time spent executing kernel services for the process. */
        uint64_t residentMemoryBytes = 0;         /**< Physical memory resident when the observation was captured. */
        uint64_t peakResidentMemoryBytes = 0;     /**< Maximum resident physical memory observed by the operating system. */
        friend constexpr bool operator==(const ProcessUsage&, const ProcessUsage&) noexcept = default;
    };

    /** @brief Return the calling process's stable native identifier. */
    [[nodiscard]] Result<NativeProcessId> CurrentNativeProcessId() noexcept;

    /** @brief Return the native identifier recorded for the calling process's parent. */
    [[nodiscard]] Result<NativeProcessId> ParentNativeProcessId() noexcept;

    /**
     * @brief Capture the native thread identifiers currently owned by @p process.
     * @details The result is sorted and duplicate-free. Native enumeration is not transactional: threads may start or
     * exit while the operating system captures the snapshot, so consumers must reconcile it with lifecycle events.
     * @param[in] process Nonzero process identifier to inspect.
     * @return Native thread identifiers, or an argument, support, permission, or native snapshot failure.
     */
    [[nodiscard]] Result<std::vector<NativeThreadId>> EnumerateProcessThreads(NativeProcessId process);

    /** @brief Return the native path of the executable image backing the calling process. */
    [[nodiscard]] Result<std::filesystem::path> CurrentProcessImagePath();

    /** @brief Return the calling process's startup argument vector as strict UTF-8 strings. */
    [[nodiscard]] Result<std::vector<std::string>> CurrentProcessArguments();

    /** @brief Capture accumulated CPU time and current/peak resident memory for the calling process. */
    [[nodiscard]] Result<ProcessUsage> CaptureCurrentProcessUsage() noexcept;

} // namespace Sora::PAL
