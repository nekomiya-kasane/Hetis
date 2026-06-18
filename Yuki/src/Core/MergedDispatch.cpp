#include <Yuki/Core/MergedDispatch.h>

namespace Yuki {

    // D15 L2: branch-light binary search over the iid-sorted snapshot. The Important-wins
    // tiebreak (D7.3) is paid only when two entries actually share an iid — the common
    // unique-iid path takes a single equality compare past the search loop. The seal-bit
    // read is `entries[i].seal.important` (post-T13 SealFlags consolidation); the original
    // plan text referenced a loose `.important` field that no longer exists.
    const DispatchEntry* LookupMergedDispatch(
            const MergedDispatchSnapshot* snap, Iid iid) noexcept {
        if (!snap || snap->count == 0 || snap->entries == nullptr) {
            return nullptr;
        }
        std::size_t lo = 0;
        std::size_t hi = snap->count;
        while (lo < hi) {
            std::size_t mid = lo + (hi - lo) / 2;
            const DispatchEntry& e = snap->entries[mid];
            if (e.iid < iid) {
                lo = mid + 1;
            } else if (iid < e.iid) {
                hi = mid;
            } else {
                // Walk left to the start of the equal-iid run, then scan for Important.
                std::size_t start = mid;
                while (start > 0 && snap->entries[start - 1].iid == iid) --start;
                std::size_t end = mid + 1;
                while (end < snap->count && snap->entries[end].iid == iid) ++end;
                for (std::size_t i = start; i < end; ++i) {
                    if (snap->entries[i].seal.important) return &snap->entries[i];
                }
                return &snap->entries[start];
            }
        }
        return nullptr;
    }

} // namespace Yuki
