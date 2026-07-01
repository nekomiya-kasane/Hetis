/**
 * @file Introspection.cpp
 * @brief T21 — implementations route through @c node->MetaDyn() to reach @c links and
 *        @c mergedDispatch, take an @ref RcuReadGuard on entry, and binary-search the
 *        published snapshot (D15 L2 layer).
 */
#include <Yuki/Core/Introspection.h>
#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/RootObject.h>

namespace Yuki {

    namespace {

        // Resolve @p node's MetaLinks via the virtual @c MetaDyn() accessor; returns nullptr
        // for sentinel/base RootObject (no Y_OBJECT installed).
        const MetaLinks* LinksOf(const RootObject* node) noexcept {
            if (!node) {
                return nullptr;
            }
            const MetaDynamic& md = node->MetaDyn();
            return md.links;
        }

    } // namespace

    std::span<const DispatchEntry> IidsOf(const RootObject* node) noexcept {
        const MetaLinks* links = LinksOf(node);
        if (!links) {
            return {};
        }
        RcuReadGuard g;
        // Snapshot-at-call: load mergedDispatch under the guard so TryReclaim cannot free
        // it before we return — the span we hand back is valid for the guard's lifetime.
        // §2 rule 1: extensions installed AFTER this load are not visible to this call.
        const MergedDispatchSnapshot* snap = links->mergedDispatch.load(std::memory_order_acquire);
        if (!snap || !snap->entries) {
            return {};
        }
        return std::span<const DispatchEntry>(snap->entries, snap->count);
    }

    bool Provides(const RootObject* node, Iid iid) noexcept {
        const MetaLinks* links = LinksOf(node);
        if (!links) {
            return false;
        }
        RcuReadGuard g;
        const MergedDispatchSnapshot* snap = links->mergedDispatch.load(std::memory_order_acquire);
        return LookupMergedDispatch(snap, iid) != nullptr;
    }

    const MetaCore* ProviderClass(const RootObject* node, Iid iid) noexcept {
        const MetaLinks* links = LinksOf(node);
        if (!links) {
            return nullptr;
        }
        RcuReadGuard g;
        const MergedDispatchSnapshot* snap = links->mergedDispatch.load(std::memory_order_acquire);
        const DispatchEntry* e = LookupMergedDispatch(snap, iid);
        return e ? e->providerClass : nullptr;
    }

    ClassType RoleOf(const RootObject* node, Iid iid) noexcept {
        const MetaCore* pc = ProviderClass(node, iid);
        return pc ? pc->role : ClassType::None;
    }

    const MetaCore* TypeOf(const RootObject* node) noexcept {
        if (!node) {
            return nullptr;
        }
        return node->MetaDyn().core;
    }

} // namespace Yuki
