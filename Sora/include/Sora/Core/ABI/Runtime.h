/**
 * @file Runtime.h
 * @brief Lightweight runtime services normalized across supported C++ ABIs.
 * @ingroup Core
 *
 * @details Runtime ABI services are isolated from the reflection-driven name-mangling facilities in @c ABI.h so
 * low-level utilities do not inherit reflection headers or compile-time machinery. On an MSVC ABI target backed by
 * libc++, active exception propagation belongs to VCRT and must be queried through its ABI entry point.
 */
#pragma once

#include <Sora/Platform.h>

#include <exception>

namespace Sora::ABI {

    /** @cond INTERNAL */
    namespace Detail {

        extern "C" int __uncaught_exceptions();

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Return the number of exceptions currently being propagated on this thread.
     *
     * @details The MSVC ABI path queries VCRT directly. The libc++ implementation of @c std::uncaught_exceptions does
     * not observe VCRT's per-thread exception state in the current mixed runtime configuration. Other ABI families use
     * the standard-library operation.
     */
    [[nodiscard]] inline int UncaughtExceptionCount() noexcept {
        if constexpr (Platform::kIsWindows && Platform::kAbiFamily == Platform::AbiFamily::MSVC) {
            return Detail::__uncaught_exceptions();
        } else {
            return std::uncaught_exceptions();
        }
    }

} // namespace Sora::ABI

