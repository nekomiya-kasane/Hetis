/**
 * @file Thread.h
 * @brief Operating-system thread metadata and current-thread introspection absent from the C++ standard library.
 * @details
 * The standard library owns thread lifetime, cancellation, synchronization, waiting, and sleeping. This PAL surface
 * complements it with native identity, UTF-8 debugger names, current logical-processor location, and current stack
 * bounds:
 * @code{.cpp}
 * auto named = Sora::PAL::SetCurrentThreadName("Render.Submit");
 * if (!named) {
 *     HandleThreadMetadataError(named.error());
 * }
 *
 * const auto processor = Sora::PAL::CurrentLogicalProcessor();
 * if (processor) {
 *     RecordProcessorLocation(*processor);
 * }
 * const int frameMarker = 0;
 * const auto stack = Sora::PAL::CurrentThreadStackBounds();
 * if (stack && stack->Contains(&frameMarker)) {
 *     RecordStackResidentDiagnostic();
 * }
 * @endcode
 *
 * Scheduling priority, quality-of-service classes, and CPU affinity are deliberately excluded. Windows processor
 * groups, Linux CPU sets, and Darwin affinity tags have different semantics and require a separate policy-level API.
 * @ingroup PAL
 */
#pragma once

#include "Sora/ErrorCode.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Sora::PAL {

    /** @brief Maximum UTF-8 byte length accepted by every supported native thread-naming API. */
    inline constexpr size_t kPortableThreadNameMaxBytes = 15;

    /** @brief Platform-neutral spelling of one logical processor location. */
    struct LogicalProcessorLocation {
        uint32_t group = 0; /**< Windows processor group, or zero on platforms without processor groups. */
        uint32_t index = 0; /**< Logical processor index within @ref group. */

        friend bool operator==(const LogicalProcessorLocation&, const LogicalProcessorLocation&) noexcept = default;
    };

    /** @brief Half-open virtual-address interval occupied by the calling thread's current stack. */
    struct ThreadStackBounds {
        uintptr_t lower = 0; /**< Lowest address in the stack allocation. */
        uintptr_t upper = 0; /**< One-past-highest address in the stack allocation. */

        /** @brief Return the stack allocation size in bytes. */
        [[nodiscard]] constexpr size_t Size() const noexcept {
            return lower <= upper ? static_cast<size_t>(upper - lower) : 0;
        }

        /** @brief Return whether @p address lies in the half-open interval @c [lower,upper). */
        [[nodiscard]] bool Contains(const void* address) const noexcept {
            const uintptr_t value = reinterpret_cast<uintptr_t>(address);
            return lower <= value && value < upper;
        }

        friend bool operator==(const ThreadStackBounds&, const ThreadStackBounds&) noexcept = default;
    };

    /**
     * @brief Return the calling thread's operating-system identifier.
     * @details The identifier is process-local and may be reused after a thread exits. It is suitable for correlating
     * Sora diagnostics with native debuggers, profilers, and Trace Event consumers.
     * @return A nonzero native identifier on supported platforms, or zero when the platform cannot provide one.
     */
    [[nodiscard]] uint64_t CurrentNativeThreadId() noexcept;

    /**
     * @brief Assign a UTF-8 debugger/profiler name to the calling thread.
     * @param[in] name Name without embedded nulls. Native byte limits are 15 on Linux and 63 on macOS.
     * @return Success, @ref ErrorCode::InvalidThreadName, @ref ErrorCode::ThreadNameTooLong,
     * @ref ErrorCode::ThreadNativeFailure, or @ref ErrorCode::NotSupported.
     */
    [[nodiscard]] Result<void> SetCurrentThreadName(std::string_view name);

    /**
     * @brief Read the calling thread's debugger/profiler name as UTF-8.
     * @return The current name, which may be empty, or a thread/native-text error.
     */
    [[nodiscard]] Result<std::string> CurrentThreadName();

    /**
     * @brief Query the logical processor currently executing the calling thread.
     * @details The result is an observation, not affinity: the scheduler may migrate the thread immediately afterward.
     * @return Current processor location, @ref ErrorCode::ThreadNativeFailure, or @ref ErrorCode::NotSupported.
     */
    [[nodiscard]] Result<LogicalProcessorLocation> CurrentLogicalProcessor() noexcept;

    /**
     * @brief Query the calling thread's current stack allocation as a half-open address interval.
     * @details The result describes the current native stack. Code running on fibers or user-mode scheduled stacks must
     * query again after switching execution contexts.
     * @return Valid bounds with @c lower < upper, @ref ErrorCode::ThreadNativeFailure, or
     * @ref ErrorCode::NotSupported.
     */
    [[nodiscard]] Result<ThreadStackBounds> CurrentThreadStackBounds() noexcept;

} // namespace Sora::PAL
