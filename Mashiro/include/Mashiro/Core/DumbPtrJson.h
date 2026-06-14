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
 * @par How the hook is attached
 * The hook is a **partial specialisation** of the @ref Mashiro::Hook::ToJsonHook open customisation
 * point, `Hook::ToJsonHook<DumbPtr<W>>`, providing only the emit half (`ToJson`) and no `FromJson`.
 * A partial specialisation is the one mechanism that can target the whole `DumbPtr<W>` *family* at
 * once — a free ADL `ToJson` overload cannot live in `namespace Mashiro` (it would collide with the
 * `Mashiro::ToJson` customisation-point object), and a member hook would drag the nlohmann
 * dependency into `DumbPtr.h`. `Json::Detail::ToJsonImpl` picks this up via its highest-priority
 * `HasHookToJson` branch.
 *
 * @ingroup Core
 */
#pragma once

#include <format>
#include <string>

#include "Mashiro/Core/DumbPtr.h"
#include "Mashiro/Core/ToJson.h"

namespace Mashiro::Hook {

    /**
     * @brief One-way JSON hook for `DumbPtr` — emits the observed address.
     *
     * Emits `null` for an empty observer, otherwise a JSON string of the form `"0xADDR"`. Never
     * dereferences the pointee. Defines no `FromJson`, so a `DumbPtr` is output-only by design.
     *
     * @tparam W The observed (pointed-to) type.
     */
    template<class W>
    struct ToJsonHook<DumbPtr<W>> {
        [[nodiscard]] static json ToJson(DumbPtr<W> p) {
            if (!p) {
                return json(nullptr);
            }
            return json(std::format("{}", static_cast<const void*>(p.Get())));
        }
    };

} // namespace Mashiro::Hook
