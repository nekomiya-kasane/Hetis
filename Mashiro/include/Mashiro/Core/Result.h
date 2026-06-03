/**
 * @file Result.h
 * @brief Alias for `std::expected<T, ErrorCode>` — the universal fallible return type.
 *
 * All fallible Mashiro operations return `Result<T>`. Monadic chaining:
 * - `.and_then()`  — chain on success.
 * - `.transform()` — map the success value.
 * - `.or_else()`   — handle / recover from error.
 *
 * @ingroup Core
 */
#pragma once

#include <expected>

#include "Mashiro/Core/ErrorCode.h"

namespace Mashiro {

    /** 
     * @brief Fallible result type wrapping std::expected.
     * @tparam T The success value type.
     */
    template <typename T>
    using Result = std::expected<T, ErrorCode>;

    /** @brief Void result for operations that succeed with no value. */
    using VoidResult = std::expected<void, ErrorCode>;

}  // namespace Mashiro
