/**
 * @file Meta.cpp
 * @brief Metaclass provider lookup implementation.
 * @ingroup Core
 */
#include "Yuki/Core/Meta.h"

namespace Yuki {

    /** @brief Find the highest-priority direct provider for @p iid in this class. */
    [[nodiscard]] const ProviderEntry* MetaClass::FindProvide(Iid iid) const noexcept {
        const ProviderEntry* best = nullptr;
        for (const ProviderEntry& entry : core_.provides) {
            if (entry.interfaceIid == iid && (!best || entry.priority < best->priority)) {
                best = &entry;
            }
        }
        return best;
    }

} // namespace Yuki
