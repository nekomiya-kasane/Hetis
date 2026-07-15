/**
 * @file RefPtrJson.h
 * @brief Opt-in, one-way JSON integration for @ref Sora::RefPtr.
 * @ingroup Core
 *
 * @details Bridges @ref Sora::RefPtr into the reflection-driven @ref Sora::ToJson framework without forcing the
 * nlohmann/json dependency onto @c RefPtr.h. Include this header when borrowed pointer identity should appear in
 * diagnostic JSON output, for example as a member of a reflected object dumped for inspection.
 *
 * @par Why one-way
 * @ref RefPtr carries identity semantics: its JSON form is the observed address, a debug artifact that is meaningful
 * only inside the current process and run. Serializing it is useful for diagnostics; deserializing it would fabricate
 * a pointer with no valid lifetime proof. This header therefore emits @c null for an empty observer or an address
 * string for a non-null observer, and deliberately provides no @c FromJson hook.
 */
#pragma once

#include <format>
#include <string>

#include "Sora/Core/RefPtr.h"
#include "Sora/Core/ToJson.h"

namespace Sora::Hook {

    /**
     * @brief One-way JSON hook for @ref Sora::RefPtr.
     * @tparam T Observed element type.
     */
    template<typename T>
    struct ToJsonHook<RefPtr<T>> {
        /** @brief Emit @c null or a display-only address string. Never dereferences @p pointer. */
        [[nodiscard]] static Json ToJson(RefPtr<T> pointer) {
            if (!pointer) {
                return Json(nullptr);
            }
            return Json(std::format("{}", Detail::AddressForDisplay(pointer.Get())));
        }
    };

} // namespace Sora::Hook
