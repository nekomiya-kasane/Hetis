/**
 * @file PlatformBackendLinux.cpp
 * @brief Linux implementation placeholder for @ref Mashiro::Platform::PlatformBackend.
 */
#include "Mashiro/Platform/PlatformBackend.h"
#include "Mashiro/Platform/Common.h"

#ifdef PLATFORM_LINUX

namespace Mashiro {

    namespace Backend {

        void Initialize() {}

    } // namespace Backend

} // namespace Mashiro

namespace Mashiro::Platform {

    struct PlatformBackend::Impl {};

    PlatformBackend::PlatformBackend() : impl_(std::make_unique<Impl>()) {}
    PlatformBackend::~PlatformBackend() = default;

    void PlatformBackend::WaitForAnySource(stdexec::inplace_stop_token stop) noexcept {
        (void)stop;
    }

    void PlatformBackend::Wake() noexcept {}

    void PlatformBackend::AttachWindowRegistry(WindowManager& windows) noexcept {
        (void)windows;
    }

    void PlatformBackend::DrainNative(SystemEventConsumerRef consume) noexcept {
        (void)consume;
    }

} // namespace Mashiro::Platform

#endif