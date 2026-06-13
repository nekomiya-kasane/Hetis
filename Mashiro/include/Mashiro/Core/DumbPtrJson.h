/**
 * @file DumbPtrJson.h
 * @brief Opt-in, one-way JSON integration for @ref Mashiro::DumbPtr.
 *
 * Bridges @ref Mashiro::DumbPtr into the reflection-driven `ToJson` framework without forcing the
 * nlohmann dependency onto `DumbPtr.h`. Include this header (instead of, or in addition to,
 * `DumbPtr.h`) when you want a `DumbPtr` to serialise — for example as a member of a reflected
 * struct that you dump for logging or inspection.
 *
 * @par Why one-way
 * `DumbPtr` carries **identity (address) semantics**: its JSON form is the observed address, a
 * *debug artifact* that is meaningless in any other process or run. Serialising it is useful for
 * inspection; pretending it round-trips would be a lie. So this header provides emission only —
 * `null` for an empty observer, otherwise the address as a JSON string — and deliberately defines
 * **no** `FromJson`. A `DumbPtr` member in a serialised struct is therefore output-only, the
 * truthful model for a non-owning observer.
 *
 * The hook is an ADL `ToJson(DumbPtr<W>)` overload in `namespace Mashiro`, picked up by
 * `Json::Detail::ToJsonImpl`'s `HasFreeToJson` branch before any generic handling.
 *
 * @ingroup Core
 */
#pragma once

#include <format>
#include <string>

#include "Mashiro/Core/DumbPtr.h"
#include "Mashiro/Core/ToJson.h"

namespace Mashiro {

    /**
     * @brief One-way JSON hook for `DumbPtr` — emits the observed address.
     *
     * @return `json(nullptr)` for an empty observer, otherwise a JSON string of the form
     *                         `"0xADDR"`. Never dereferences the pointee.
     */
    template<class W>
    [[nodiscard]] inline json ToJson(DumbPtr<W> p) {
        if (!p) {
            return json(nullptr);
        }
        return json(std::format("{}", static_cast<const void*>(p.Get())));
    }

} // namespace Mashiro
