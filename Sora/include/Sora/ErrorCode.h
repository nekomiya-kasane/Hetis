/**
 * @file ErrorCode.h
 * @brief Engine-wide error code enumeration with per-module ranges.
 * @ingroup Core
 */
#pragma once

#include <cstdint>
#include <expected>
#include <optional>

namespace Sora {

    /**
     * @brief Universal error code returned by fallible engine operations.
     *
     * Each sub-system owns a disjoint 4096-value range so that new codes can be
     * added without cross-module coordination:
     *
     * | Module             | Range                |
     * |--------------------|----------------------|
     * | Common             | `0x0001` – `0x00FF`  |
     * | Core Rendering     | `0x1000` – `0x1FFF`  |
     * | Mesh Shader        | `0x2000` – `0x2FFF`  |
     * | GPU HLR            | `0x3000` – `0x3FFF`  |
     * | Ray Tracing Pick   | `0x4000` – `0x4FFF`  |
     * | Resource Management| `0x5000` – `0x5FFF`  |
     * | Coca Coroutine     | `0x6000` – `0x6FFF`  |
     * | Kernel             | `0x7000` – `0x7FFF`  |
     * | Debug Infra        | `0xD000` – `0xDFFF`  |
     * | GPU Device         | `0xF000` – `0xFFFF`  |
     */
    enum class [[nodiscard]] ErrorCode : uint16_t {

        /** @name Success @{ */
        Ok = 0, ///< Operation completed successfully.
        /** @} */

        /** @name Common (0x0001 – 0x00FF) @{ */
        InvalidArgument = 0x0001,      ///< A supplied argument is out of range or malformed.
        OutOfMemory = 0x0002,          ///< Host or device memory allocation failed.
        NotSupported = 0x0003,         ///< Requested feature is not supported on this platform.
        NotImplemented = 0x0004,       ///< Code path has not been implemented yet.
        InvalidState = 0x0005,         ///< Object is in an invalid state for the requested operation.
        IoError = 0x0006,              ///< File or stream I/O failure.
        Timeout = 0x0007,              ///< Operation did not complete within the allowed time.
        ResourceExhausted = 0x0008,    ///< A fixed-capacity resource pool is full.
        PreconditionViolated = 0x0009, ///< A documented precondition was not met.
        ModuleLoadFailed = 0x000A,     ///< Dynamic module could not be loaded.
        SectionNotFound = 0x000B,      ///< Requested section does not exist in the module image.
        DataTruncated = 0x000C,        ///< A byte stream ended before the requested data was complete.
        DataCorrupted = 0x000D,        ///< A generic byte stream failed structural or integrity checks.
        InvalidSyntax = 0x000E,        ///< Text does not conform to the target type's accepted grammar.
        OutOfRange = 0x000F,           ///< A parsed or computed value cannot be represented by the target type.

        InvalidScalar = 0x0010,           ///< Input code point is not a Unicode scalar value.
        IsolatedHighSurrogate = 0x0011,   ///< UTF-16 input contains an unpaired high surrogate.
        IsolatedLowSurrogate = 0x0012,    ///< UTF-16 input contains an unpaired low surrogate.
        InvalidUtf8LeadingByte = 0x0013,  ///< UTF-8 input starts with an invalid leading byte.
        InvalidUtf8Continuation = 0x0014, ///< UTF-8 input contains an invalid continuation byte.
        OverlongUtf8Sequence = 0x0015,    ///< UTF-8 input uses a non-shortest encoding.
        TruncatedUtf8Sequence = 0x0016,   ///< UTF-8 input ends before the current scalar is complete.

        EmptyConfigurationPath = 0x0017,            ///< A configuration path is empty.
        EmptyConfigurationPathSegment = 0x0018,     ///< A configuration path contains an empty segment.
        InvalidConfigurationPathCharacter = 0x0019, ///< A configuration path contains a non-portable character.
        InvalidEnvironmentName = 0x001A,            ///< An environment-variable name is malformed or not representable.
        InvalidEnvironmentValue = 0x001B,      ///< An environment-variable value is malformed or not representable.
        InvalidNativeEnvironmentText = 0x001C, ///< Native environment storage is not valid Unicode text.
        EnvironmentNativeFailure = 0x001D,     ///< A native process-environment operation failed.
        DuplicateEnvironmentMutation = 0x001E, ///< A transaction mutates one environment name more than once.
        EnvironmentRollbackFailure = 0x001F,   ///< A failed environment transaction could not restore prior state.

        ClipboardBusy = 0x0020,          ///< Another process retained exclusive clipboard access.
        InvalidClipboardData = 0x0021,   ///< Native clipboard text violates its storage or encoding invariants.
        ClipboardTextTooLarge = 0x0022,  ///< Clipboard text cannot fit the platform-native representation.
        ClipboardNativeFailure = 0x0023, ///< A native clipboard or global-memory operation failed.

        InvalidThreadName = 0x0024,       ///< A thread name is not valid UTF-8 or contains an embedded null.
        ThreadNameTooLong = 0x0025,       ///< A thread name exceeds the native platform limit.
        ThreadNativeFailure = 0x0026,     ///< A native thread metadata or introspection operation failed.
        InvalidNativeThreadText = 0x0027, ///< Native thread metadata is not valid Unicode text.
        AlreadyExists = 0x0028,           ///< Atomic creation failed because the destination already exists.
        ProcessNativeFailure = 0x0029,     ///< A native current-process introspection operation failed.
        InvalidNativeProcessText = 0x002A, ///< Native process metadata is not valid Unicode text.
        /** @} */

        /** @name Core Rendering (0x1000 – 0x1FFF) @{ */
        PipelineCreationFailed = 0x1000,  ///< Graphics or compute pipeline creation failed.
        ShaderCompilationFailed = 0x1001, ///< Shader module compilation or linking failed.
        RenderPassInvalid = 0x1002,       ///< Render pass configuration is invalid.
        GraphCycleDetected = 0x1003,      ///< Frame-graph contains a dependency cycle.
        UnresolvedResource = 0x1004,      ///< A frame-graph resource has no producer.
        /** @} */

        /** @name Mesh Shader (0x2000 – 0x2FFF) @{ */
        MeshletOverflow = 0x2000, ///< Meshlet vertex/primitive count exceeds HW limit.
        /** @} */

        /** @name GPU Hidden-Line Removal (0x3000 – 0x3FFF) @{ */
        HlrBufferOverflow = 0x3000, ///< HLR intermediate buffer capacity exceeded.
        /** @} */

        /** @name Ray Tracing Pick (0x4000 – 0x4FFF) @{ */
        RayMiss = 0x4000, ///< Pick ray did not intersect any geometry.
        /** @} */

        /** @name Resource Management (0x5000 – 0x5FFF) @{ */
        ResourceNotFound = 0x5000,  ///< Requested resource does not exist.
        ResourceCorrupted = 0x5001, ///< Resource data failed integrity checks.
        /** @} */

        /** @name Coca Coroutine (0x6000 – 0x6FFF) @{ */
        TaskCancelled = 0x6000, ///< Async task was cancelled before completion.
        /** @} */

        /** @name Kernel (0x7000 – 0x7FFF) @{ */
        ImportFailed = 0x7000,       ///< Asset import failed (format or codec error).
        TessellationFailed = 0x7001, ///< Surface tessellation produced degenerate output.
        ExportFailed = 0x7002,       ///< Asset export failed (I/O or encoding error).
        /** @} */

        /** @name Debug Infrastructure (0xD000 – 0xDFFF) @{ */
        LogSinkFull = 0xD000,               ///< Log ring-buffer is full; messages dropped.
        LogFileOpenFailed = 0xD001,         ///< Could not open/create the log file.
        CrashHandlerInstallFailed = 0xD002, ///< OS crash handler registration failed.
        EmergencyPathInvalid = 0xD003,      ///< Emergency dump path is inaccessible.
        BreadcrumbBufferFull = 0xD004,      ///< GPU breadcrumb buffer capacity exceeded.
        ShaderPrintfOverflow = 0xD005,      ///< Shader printf output buffer overflowed.
        CaptureToolNotAvailable = 0xD006,   ///< Frame-capture tool (RenderDoc, PIX) not attached.
        ProfilerNotInitialized = 0xD007,    ///< GPU profiler was not initialised.
        TraceExportFailed = 0xD008,         ///< Performance trace export failed.
        /** @} */

        /** @name GPU Device (0xF000 – 0xFFFF) @{ */
        DeviceLost = 0xF000,         ///< GPU device has been lost (TDR or HW error).
        DeviceNotReady = 0xF001,     ///< GPU device is not yet ready for use.
        SwapchainOutOfDate = 0xF002, ///< Swapchain needs recreation (resize / DPI change).
        SurfaceLost = 0xF003,        ///< Window surface is no longer valid.
        /** @} */

        Unknown = 0xFFFF, ///< Unknown error code (used for unexpected native failures).
    };

    /**
     * @brief Fallible result type wrapping std::expected.
     * @tparam T The success value type.
     */
    template<typename T>
    using Result = std::expected<T, ErrorCode>;

    /** @brief Void result for operations that succeed with no value. */
    using VoidResult = std::expected<void, ErrorCode>;

    /** @brief Converts an std::optional to a Result, returning ErrorCode::Unknown if the optional is empty. */
    template<typename T>
    [[nodiscard]] constexpr Result<T> NormalizeResult(std::optional<T>&& result) {
        if (!result) {
            return std::unexpected(ErrorCode::Unknown);
        }

        return std::move(*result);
    }

} // namespace Sora
