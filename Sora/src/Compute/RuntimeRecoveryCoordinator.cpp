/**
 * @file RuntimeRecoveryCoordinator.cpp
 * @brief Construction of process-local runtime recovery coordinators.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/RuntimeRecoveryCoordinator.h>

#include <new>

namespace Sora::Compute {

    std::expected<std::unique_ptr<RuntimeRecoveryCoordinator>, RuntimeRecoveryError>
    RuntimeRecoveryCoordinator::Create(RuntimeEpoch initial) noexcept {
        try {
            auto epoch = std::make_shared<const RuntimeEpoch>(std::move(initial));
            return std::unique_ptr<RuntimeRecoveryCoordinator>{new RuntimeRecoveryCoordinator{std::move(epoch)}};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeRecoveryErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute
