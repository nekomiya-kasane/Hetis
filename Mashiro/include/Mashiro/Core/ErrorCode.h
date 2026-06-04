/**
 * @file ErrorCode.h
 * @brief Engine-wide error code enumeration with per-module ranges.
 * @ingroup Core
 */
#pragma once

#include <cstdint>
#include <meta>

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
enum class ErrorCode : uint16_t {

    /// @name Success
    /// @{
    Ok = 0, ///< Operation completed successfully.
    /// @}

    /// @name Common (0x0001 – 0x00FF)
    /// @{
    InvalidArgument     = 0x0001, ///< A supplied argument is out of range or malformed.
    OutOfMemory         = 0x0002, ///< Host or device memory allocation failed.
    NotSupported        = 0x0003, ///< Requested feature is not supported on this platform.
    NotImplemented      = 0x0004, ///< Code path has not been implemented yet.
    InvalidState        = 0x0005, ///< Object is in an invalid state for the requested operation.
    IoError             = 0x0006, ///< File or stream I/O failure.
    Timeout             = 0x0007, ///< Operation did not complete within the allowed time.
    ResourceExhausted   = 0x0008, ///< A fixed-capacity resource pool is full.
    PreconditionViolated= 0x0009, ///< A documented precondition was not met.
    /// @}

    /// @name Core Rendering (0x1000 – 0x1FFF)
    /// @{
    PipelineCreationFailed  = 0x1000, ///< Graphics or compute pipeline creation failed.
    ShaderCompilationFailed = 0x1001, ///< Shader module compilation or linking failed.
    RenderPassInvalid       = 0x1002, ///< Render pass configuration is invalid.
    GraphCycleDetected      = 0x1003, ///< Frame-graph contains a dependency cycle.
    UnresolvedResource      = 0x1004, ///< A frame-graph resource has no producer.
    /// @}

    /// @name Mesh Shader (0x2000 – 0x2FFF)
    /// @{
    MeshletOverflow = 0x2000, ///< Meshlet vertex/primitive count exceeds HW limit.
    /// @}

    /// @name GPU Hidden-Line Removal (0x3000 – 0x3FFF)
    /// @{
    HlrBufferOverflow = 0x3000, ///< HLR intermediate buffer capacity exceeded.
    /// @}

    /// @name Ray Tracing Pick (0x4000 – 0x4FFF)
    /// @{
    RayMiss = 0x4000, ///< Pick ray did not intersect any geometry.
    /// @}

    /// @name Resource Management (0x5000 – 0x5FFF)
    /// @{
    ResourceNotFound   = 0x5000, ///< Requested resource does not exist.
    ResourceCorrupted  = 0x5001, ///< Resource data failed integrity checks.
    /// @}

    /// @name Coca Coroutine (0x6000 – 0x6FFF)
    /// @{
    TaskCancelled = 0x6000, ///< Async task was cancelled before completion.
    /// @}

    /// @name Kernel (0x7000 – 0x7FFF)
    /// @{
    ImportFailed        = 0x7000, ///< Asset import failed (format or codec error).
    TessellationFailed  = 0x7001, ///< Surface tessellation produced degenerate output.
    ExportFailed        = 0x7002, ///< Asset export failed (I/O or encoding error).
    /// @}

    /// @name Debug Infrastructure (0xD000 – 0xDFFF)
    /// @{
    LogSinkFull               = 0xD000, ///< Log ring-buffer is full; messages dropped.
    LogFileOpenFailed         = 0xD001, ///< Could not open/create the log file.
    CrashHandlerInstallFailed = 0xD002, ///< OS crash handler registration failed.
    EmergencyPathInvalid      = 0xD003, ///< Emergency dump path is inaccessible.
    BreadcrumbBufferFull      = 0xD004, ///< GPU breadcrumb buffer capacity exceeded.
    ShaderPrintfOverflow      = 0xD005, ///< Shader printf output buffer overflowed.
    CaptureToolNotAvailable   = 0xD006, ///< Frame-capture tool (RenderDoc, PIX) not attached.
    ProfilerNotInitialized    = 0xD007, ///< GPU profiler was not initialised.
    TraceExportFailed         = 0xD008, ///< Performance trace export failed.
    /// @}

    /// @name GPU Device (0xF000 – 0xFFFF)
    /// @{
    DeviceLost         = 0xF000, ///< GPU device has been lost (TDR or HW error).
    DeviceNotReady     = 0xF001, ///< GPU device is not yet ready for use.
    SwapchainOutOfDate = 0xF002, ///< Swapchain needs recreation (resize / DPI change).
    SurfaceLost        = 0xF003, ///< Window surface is no longer valid.
    /// @}
};

// Compile-time invariant: no two ErrorCode enumerators share the same value.
consteval {
    auto enums = std::meta::enumerators_of(^^ErrorCode);
    for (size_t i = 0; i < enums.size(); ++i)
        for (size_t j = i + 1; j < enums.size(); ++j)
            if (std::meta::constant_of(enums[i]) == std::meta::constant_of(enums[j]))
                throw "ErrorCode: duplicate enumerator value detected";
}
