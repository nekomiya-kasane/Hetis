/**
 * @file BackendTestSupport.h
 * @brief Process-wide owned backend service used by compute ABI unit tests.
 */
#pragma once

#include <Sora/Compute/Backend/Service.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

namespace Sora::Test {

    namespace Detail {

        struct TestBackendState {};
        struct TestCodeOwner {};

        inline void DestroyTestBackendState(void* state) noexcept {
            delete static_cast<TestBackendState*>(state);
        }

    } // namespace Detail

    /** @brief Return an actually owned process-wide service for callbacks compiled into each test executable. */
    [[nodiscard]] inline const Compute::BackendService& TestBackendService() noexcept {
        static const Compute::BackendService service = [] {
            auto state = std::make_unique<Detail::TestBackendState>();
            auto code = Compute::BackendCodeLease::Retain(std::make_shared<Detail::TestCodeOwner>());
            auto adopted = Compute::BackendService::Adopt({.id = {0x534F52415F544553ULL, 0x545F4241434B454EULL},
                                                           .state = state.get(),
                                                           .destroy = Detail::DestroyTestBackendState},
                                                          std::move(code));
            if (!adopted) {
                std::terminate();
            }
            static_cast<void>(state.release());
            return std::move(*adopted);
        }();
        return service;
    }

} // namespace Sora::Test
