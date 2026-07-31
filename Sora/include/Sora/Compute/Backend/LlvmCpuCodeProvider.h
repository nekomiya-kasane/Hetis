/**
 * @file LlvmCpuCodeProvider.h
 * @brief Optional LLVM ORC native-code provider for the built-in CPU backend.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/CpuCodeProvider.h>

#include <expected>
#include <filesystem>
#include <span>

namespace Sora::Compute {

    inline constexpr uint32_t kLlvmCpuCodeProviderMajorVersion = 22;

    /** @brief Search roots used only while resolving the optional LLVM-C runtime module. */
    struct LlvmCpuCodeProviderConfig {
        std::span<const std::filesystem::path> searchPaths{};
    };

    /**
     * @brief Load LLVM-C 22, initialize the native target, and create one shared ORC execution session.
     * @details The returned provider owns no compile-time LLVM dependency. Its code lease keeps the dynamically loaded
     * LLVM module mapped until the final native artifact has been destroyed.
     */
    [[nodiscard]] std::expected<CpuCodeProvider, CompileError>
    CreateLlvmCpuCodeProvider(LlvmCpuCodeProviderConfig config = {}) noexcept;

} // namespace Sora::Compute
