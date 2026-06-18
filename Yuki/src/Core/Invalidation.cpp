#include <Yuki/Core/ExtendedList.h>
#include <Yuki/Core/MetaLinks.h>

namespace Yuki {

    void BroadcastInvalidation(const ExtendedListSnapshot* snap) noexcept {
        if (!snap || snap->count == 0 || snap->downstreams == nullptr) return;
        for (std::size_t i = 0; i < snap->count; ++i) {
            const MetaLinks* down = snap->downstreams[i];
            if (!down) continue;
            const_cast<MetaLinks*>(down)->BumpCacheEpoch();
        }
    }

} // namespace Yuki
