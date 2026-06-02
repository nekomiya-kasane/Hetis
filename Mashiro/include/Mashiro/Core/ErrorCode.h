#pragma once

#include <cstdint>

/** @brief Universal error code enum.
 *
 * Ranges per module:
 * - Common:          0x0001 - 0x00FF
 * - Core Rendering:  0x1000 - 0x1FFF
 * - Mesh Shader:     0x2000 - 0x2FFF
 * - GPU HLR:         0x3000 - 0x3FFF
 * - Ray Tracing Pick:0x4000 - 0x4FFF
 * - Resource Mgmt:   0x5000 - 0x5FFF
 * - Coca Coroutine:  0x6000 - 0x6FFF
 * - GPU Device:      0xF000 - 0xFFFF
 */
enum class ErrorCode : uint16_t {
    // --- Success ---
    Ok = 0,

    // --- Common (0x0001 - 0x00FF) ---
    InvalidArgument = 0x0001,
    OutOfMemory = 0x0002,
    NotSupported = 0x0003,
    NotImplemented = 0x0004,
    InvalidState = 0x0005,
    IoError = 0x0006,
    Timeout = 0x0007,
    ResourceExhausted = 0x0008,
    PreconditionViolated = 0x0009,

    // --- Core Rendering (0x1000 - 0x1FFF) ---
    PipelineCreationFailed = 0x1000,
    ShaderCompilationFailed = 0x1001,
    RenderPassInvalid = 0x1002,
    GraphCycleDetected = 0x1003,
    UnresolvedResource = 0x1004,

    // --- Mesh Shader (0x2000 - 0x2FFF) ---
    MeshletOverflow = 0x2000,

    // --- GPU HLR (0x3000 - 0x3FFF) ---
    HlrBufferOverflow = 0x3000,

    // --- Ray Tracing Pick (0x4000 - 0x4FFF) ---
    RayMiss = 0x4000,

    // --- Resource Mgmt (0x5000 - 0x5FFF) ---
    ResourceNotFound = 0x5000,
    ResourceCorrupted = 0x5001,

    // --- Coca Coroutine (0x6000 - 0x6FFF) ---
    TaskCancelled = 0x6000,

    // --- Kernel (0x7000 - 0x7FFF) ---
    ImportFailed = 0x7000,
    TessellationFailed = 0x7001,
    ExportFailed = 0x7002,

    // --- Debug Infrastructure (0xD000 - 0xDFFF) ---
    LogSinkFull = 0xD000,
    LogFileOpenFailed = 0xD001,
    CrashHandlerInstallFailed = 0xD002,
    EmergencyPathInvalid = 0xD003,
    BreadcrumbBufferFull = 0xD004,
    ShaderPrintfOverflow = 0xD005,
    CaptureToolNotAvailable = 0xD006,
    ProfilerNotInitialized = 0xD007,
    TraceExportFailed = 0xD008,

    // --- GPU Device (0xF000 - 0xFFFF) ---
    DeviceLost = 0xF000,
    DeviceNotReady = 0xF001,
    SwapchainOutOfDate = 0xF002,
    SurfaceLost = 0xF003,
};
