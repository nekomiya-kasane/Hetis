/**
 * @file Registry.cpp
 * @brief Out-of-line definitions for the registrar bookkeeping declared in
 *        @ref Yuki/Core/Registry.h.
 *
 * Three process-wide tables, all function-static for trivial init order:
 *  - The installed set (membership keyed by @ref Yuki::Iid) — uses @c std::set because @c Iid
 *    already provides @c operator<=> but no @c std::hash specialization. A 128-bit key has plenty
 *    of ordering work to amortize the tree-vs-table difference, and the set is touched at
 *    registrar startup only, not on the query hot path.
 *  - The per-metaclass writer-mutex map — uses @c std::map (node-based) so references handed out
 *    by @ref Yuki::Registry::WriterMutexFor stay valid even as new classes register and grow the
 *    map. @c std::unordered_map would rehash and invalidate the references, and @c std::mutex is
 *    not movable so wrapping in @c unique_ptr would add indirection for no win.
 *  - A short top-level @c std::mutex guards each table; critical sections are a lookup, an insert,
 *    or a read — tiny enough that no spinning helper buys anything over the standard primitive.
 *
 * Spec §3.3 step 3–4 — the bookkeeping that @c Install (Tasks 7–8) layers Install logic on top of.
 */
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MetaClass.h>

#include <map>
#include <set>

namespace Yuki::Registry {

namespace {

    /// @brief Mutex guarding @ref InstalledSet. Function-static — no SIOF risk, first touch inits.
    std::mutex& InstalledMutex() noexcept {
        static std::mutex m;
        return m;
    }

    /// @brief The set of installed @ref Iid values. Ordered (uses @c Iid::operator<=>) because
    ///        no @c std::hash specialization exists for @ref Iid.
    std::set<Iid>& InstalledSet() noexcept {
        static std::set<Iid> s;
        return s;
    }

    /// @brief Mutex guarding @ref MutexMap. Distinct from @ref InstalledMutex so the two tables
    ///        do not serialize through one another — the installed-set check and the
    ///        writer-mutex lookup are independent steps in the registrar.
    std::mutex& MutexMapMutex() noexcept {
        static std::mutex m;
        return m;
    }

    /// @brief Per-metaclass writer mutex table. @c std::map is required (not @c unordered_map):
    ///        node-based storage keeps the returned @c std::mutex addresses stable across later
    ///        inserts, and @c std::mutex is non-movable so the bucket-based container would have
    ///        to indirect through @c unique_ptr for no benefit.
    std::map<const MetaClass*, std::mutex>& MutexMap() noexcept {
        static std::map<const MetaClass*, std::mutex> mp;
        return mp;
    }

} // namespace

bool AlreadyInstalled(Iid id) noexcept {
    std::lock_guard guard{InstalledMutex()};
    return InstalledSet().contains(id);
}

void MarkInstalled(Iid id) noexcept {
    std::lock_guard guard{InstalledMutex()};
    InstalledSet().insert(id);
}

std::mutex& WriterMutexFor(const MetaClass& mc) noexcept {
    std::lock_guard guard{MutexMapMutex()};
    // operator[] default-constructs the @c std::mutex on first lookup — the @c std::map node
    // is permanent, so the reference returned here will remain valid even as later registrars
    // grow the map.
    return MutexMap()[&mc];
}

} // namespace Yuki::Registry
