/**
 * @file Registry.h
 * @brief Process-wide registrar bookkeeping for the Yuki closure model — spec §3.3 step 3–4.
 *
 * The registrar runs once per class (driven by Task 9's CRTP hook). Before publishing a new
 * snapshot it must:
 *
 *   - Check @ref Yuki::Registry::AlreadyInstalled to make duplicate registrar invocations a
 *     no-op (the same TU can run twice — once in the host exe and once in a plugin DLL whose
 *     class lists overlap).
 *   - Take @ref Yuki::Registry::WriterMutexFor for the metaclass it is about to publish, so two
 *     writers racing on the *same* class serialize while writers on different classes proceed
 *     concurrently. The mutex is per-metaclass (not global) precisely so independent installs
 *     do not fight over a shared lock at startup.
 *
 * The body of @c Install lands in @ref Yuki/Core/Registry.h (Tasks 7–8); this header declares
 * only the bookkeeping primitives. All functions are noexcept — registrars run during static
 * init and cannot throw without taking down the process.
 */
#pragma once

#include <Yuki/Core/FacadeList.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/RootObject.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Yuki {

    namespace Registry {

        /**
         * @brief Has @p id already been registered? Idempotency gate for the registrar.
         *
         * Returns @c true iff a prior @ref MarkInstalled call recorded @p id. Cheap to call (a
         * short mutex-guarded set lookup) — readers may also use it to test "does this class
         * even exist yet" during diagnostic paths.
         */
        [[nodiscard]] bool AlreadyInstalled(Iid id) noexcept;

        /**
         * @brief Record @p id as installed. Idempotent — duplicate calls are no-ops.
         *
         * Called by the registrar after a successful publish so subsequent runs of the same
         * registrar TU short-circuit at @ref AlreadyInstalled.
         */
        void MarkInstalled(Iid id) noexcept;

        /**
         * @brief Hand out the writer mutex for @p mc. Same metaclass → same mutex, every call.
         *
         * Lazily inserts a fresh @c std::mutex into the per-metaclass map on first request.
         * The map uses @c std::map (not @c unordered_map) so the returned reference stays valid
         * across later inserts — node-based maps don't rehash, so the mutex address is stable
         * for the life of the process.
         */
        [[nodiscard]] std::mutex& WriterMutexFor(const MetaClass& mc) noexcept;

    } // namespace Registry

    /// @cond INTERNAL
    namespace Detail {

        /**
         * @brief Byte offset of the @p I subobject inside @p T, via static_cast on a probe pointer.
         *
         * Used by @ref Yuki::Registry::Install to bake a @c DispatchKind::DirectCast offset into each
         * entry — the runtime dispatch is then a single integer add on the @c T* nucleus. The probe
         * address is a non-null sentinel; no memory is dereferenced, only the layout-aware pointer
         * adjustment that @c static_cast performs is observed via integer arithmetic. This is the
         * same trick the canonical CATIA TIE/BOA implementations use and matches MSVC's own
         * implementation of @c offsetof for virtual / multiple inheritance.
         */
        template<typename T, typename I>
        inline std::ptrdiff_t StaticCastOffset() noexcept {
            constexpr std::uintptr_t kProbe = 0x1000;
            T* p  = reinterpret_cast<T*>(kProbe);
            I* ip = static_cast<I*>(p);
            return static_cast<std::ptrdiff_t>(reinterpret_cast<std::uintptr_t>(ip) - kProbe);
        }

        /**
         * @brief Build the @ref DispatchEntry array for an Implementation @p T at registration time.
         *
         * Walks @c kImplementsInfos<T> (the annotation-driven static list of interfaces) and emits
         * one @ref DispatchKind::DirectCast entry per interface. The array is sorted by @ref Iid so
         * the snapshot satisfies the binary-search precondition of @ref Detail::LookupEntry. The
         * returned @c std::array has size known at compile time but values computed at first call.
         *
         * @note Only the DirectCast arm is filled here. @c InlineFacade entries — for interfaces the
         *       impl does *not* C++-inherit but instead routes through a small adapter — land in
         *       Task 11 once the static-face facade probe is in place.
         */
        template<typename T>
        inline auto BuildImplDispatchEntries() noexcept {
            constexpr auto N = kImplementsInfos<T>.size();
            std::array<DispatchEntry, N> out{};
            std::size_t i = 0;
            template for (constexpr auto Ireflect : kImplementsInfos<T>) {
                using I = [:Ireflect:];
                out[i] = DispatchEntry{
                    IidOf<I>(),
                    DispatchKind::DirectCast,
                    {.staticOffset = StaticCastOffset<T, I>()},
                };
                ++i;
            }
            std::ranges::sort(out, {}, &DispatchEntry::iid);
            return out;
        }

    } // namespace Detail
    /// @endcond

    namespace Registry {

        /**
         * @brief Publish a @ref DispatchSnapshot for the Implementation class @p T.
         *
         * Spec §3.3 step 1 + §4.2 DirectCast arm. The body is a textbook double-checked-locking
         * registrar:
         *  1. Check @ref AlreadyInstalled — fast path when the same TU runs twice.
         *  2. Take @ref WriterMutexFor — only writers on this class serialize; writers on other
         *     classes run concurrently.
         *  3. Re-check under the lock to close the race window.
         *  4. Build the entry array (function-static, one per @p T, lazy-init thread-safe by
         *     C++11 magic statics).
         *  5. Lazily install the @c MetaLinks on first call.
         *  6. Release-publish the new snapshot via @c exchange so any future reader's acquire-load
         *     sees a fully-constructed snapshot; retire the old one if any.
         *  7. @ref MarkInstalled and @ref Detail::SweepRetirements — the per-Install sweep is the
         *     RCU epoch boundary spec §2.3 describes.
         *
         * @note Function-static storage gives the snapshot @em program lifetime, so the retire
         *       deleter is a no-op — there is nothing to free, only the bookkeeping slot to drain.
         *       Snapshots backed by registry-owned arenas (Tasks 8 / 10) will use real deleters.
         */
        template<ImplementationClass T>
        inline void Install() noexcept {
            const Iid id = IidOf<T>();
            if (AlreadyInstalled(id)) return;
            auto& mc = MetaClassOf<T>;
            std::lock_guard guard{WriterMutexFor(mc)};
            if (AlreadyInstalled(id)) return;

            static MetaLinks                       links;
            static auto                            entries = Detail::BuildImplDispatchEntries<T>();
            static const DispatchSnapshot          snap{entries.size(), entries.data(), nullptr};

            if (mc.links() == nullptr) {
                mc.setLinks(&links);
            }
            const auto* old = links.dispatch.exchange(&snap, std::memory_order_release);
            if (old != nullptr) {
                Detail::RetireSnapshot(old, [](const DispatchSnapshot*) noexcept {});
            }
            MarkInstalled(id);
            Detail::SweepRetirements();
        }

    } // namespace Registry

    /// @cond INTERNAL
    namespace Detail {

        // =====================================================================
        // Extension singleton + resolver function objects (Task 8)
        // =====================================================================
        //
        // Stateless Extensions take the @c CodeExtensionSingleton arm: the DispatchEntry payload
        // points at a pointer-variable holding the singleton's address. Real singleton
        // materialisation (the @c static E gSingleton{} dance) needs facade storage that is not
        // settled until T10, so for T8 the variable is a `nullptr` sentinel. The T8 tests only
        // check the entry kind, not the singleton address — see the commit message.
        template<typename E>
        inline RootObject* const SingletonAddrFor = nullptr;

        // Stateful Extensions take the @c SideTableResolver arm. The resolver walks the nucleus's
        // facade chain and returns the first match. On a miss, the resolver should call
        // E::MaterializeInto(*n) and retry — but MaterializeInto lands in T10, so we leave the
        // miss path returning nullptr with a TODO. The T8 tests only check that the resolver is
        // non-null and the entry kind is correct.
        template<typename E>
        inline RootObject* SideTableResolverFor(RootObject* n) noexcept {
            if (n == nullptr) {
                return nullptr;
            }
            FacadeListHead* head = RT::Facades(n);
            if (head == nullptr) {
                return nullptr;
            }
            if (RootObject* hit = FacadeListLookup(*head, IidOf<E>())) {
                return hit;
            }
            // TODO(task-10): on miss, call E::MaterializeInto(*n) and retry the lookup.
            return nullptr;
        }

        /// @brief All iids advertised by an Extension @p E (from `kImplementsInfos<E>`), baked
        ///        into a static array at compile time so the runtime path never touches a
        ///        consteval-only @c std::meta::info value.
        template<typename E>
        inline constexpr auto kExtensionImplementsIids = [] consteval {
            constexpr auto N = kImplementsInfos<E>.size();
            std::array<Iid, N> out{};
            std::size_t i = 0;
            template for (constexpr auto Ireflect : kImplementsInfos<E>) {
                using I = [:Ireflect:];
                out[i++] = IidOf<I>();
            }
            return out;
        }();

        /// @brief Build one @ref DispatchEntry for @p E advertising interface @p I.
        ///        Selects @c CodeExtensionSingleton vs @c SideTableResolver from
        ///        @ref StatelessExtensionClass — the compile-time discriminator from spec §3.2.
        template<typename E, typename I>
        inline DispatchEntry MakeExtensionEntry() noexcept {
            DispatchEntry e{};
            e.iid = IidOf<I>();
            if constexpr (StatelessExtensionClass<E>) {
                e.kind = DispatchKind::CodeExtensionSingleton;
                e.payload.singleton = &SingletonAddrFor<E>;
            } else {
                e.kind = DispatchKind::SideTableResolver;
                e.payload.resolver = &SideTableResolverFor<E>;
            }
            return e;
        }

        /**
         * @brief Compose a fresh @ref DispatchSnapshot replacing @p mc's existing one, overlaying
         *        @p E's (iid, kind) entries onto the old snapshot.
         *
         * Spec §3.2 precedence rule: Extensions win over the Implementation for the same iid. We
         * implement that by dropping every old entry whose iid is in @p E's implements set, then
         * appending @p E's new entries and sorting by iid (the binary-search precondition that
         * @ref Detail::LookupEntry depends on).
         *
         * Allocation discipline: the composed snapshot and its entry array live on the heap (one
         * @c new each). The previous snapshot is handed to @ref RetireSnapshot with a no-op
         * deleter — matching the T7 Implementation path, which uses function-static storage for
         * its initial snapshot. T8 thus leaks the prior snapshot, but install is idempotent so at
         * most one snapshot is leaked per Install<E>. Real retirement is deferred until snapshots
         * can be unconditionally heap-owned (post-T10).
         */
        template<typename E, typename B>
        inline void PublishExtensionEntries(MetaClass& mc) noexcept {
            // 1. Read the existing snapshot (may be null if B was not installed by T7 yet).
            //    We may also need to install a fresh MetaLinks if B has none.
            static MetaLinks linksForB;  // one per (E, B) template — fine: same B gets same links
            if (mc.links() == nullptr) {
                mc.setLinks(&linksForB);
            }
            MetaLinks* links = mc.links();
            const DispatchSnapshot* old = links->dispatch.load(std::memory_order_acquire);

            // 2. Build the to-replace set of iids advertised by E. The array is baked at compile
            //    time, so no consteval-only values escape into runtime code.
            constexpr auto& eIids = kExtensionImplementsIids<E>;
            auto sameIid = [](Iid id) noexcept {
                for (Iid x : eIids) {
                    if (x == id) {
                        return true;
                    }
                }
                return false;
            };

            // 3. Build the new entry vector: keep old entries whose iid is NOT in E's set, then
            //    append fresh entries for each iid in E's implements list.
            std::vector<DispatchEntry> merged;
            const std::size_t oldCount = (old != nullptr) ? old->count : 0;
            merged.reserve(oldCount + eIids.size());
            for (std::size_t i = 0; i < oldCount; ++i) {
                if (!sameIid(old->entries[i].iid)) {
                    merged.push_back(old->entries[i]);
                }
            }
            template for (constexpr auto Ireflect : kImplementsInfos<E>) {
                using I = [:Ireflect:];
                merged.push_back(MakeExtensionEntry<E, I>());
            }
            std::ranges::sort(merged, {}, &DispatchEntry::iid);

            // 4. Allocate the new snapshot + entries on the heap. The arrays are POD so a plain
            //    new[] copy is sufficient.
            auto* entries = new DispatchEntry[merged.size()];
            for (std::size_t i = 0; i < merged.size(); ++i) {
                entries[i] = merged[i];
            }
            auto* fresh = new DispatchSnapshot{merged.size(), entries, old};

            // 5. Publish with release; retire the old with a no-op deleter (matches T7's static
            //    snapshot ownership model). See the function doc for the leak trade-off.
            const auto* prior = links->dispatch.exchange(fresh, std::memory_order_release);
            if (prior != nullptr) {
                RetireSnapshot(prior, [](const DispatchSnapshot*) noexcept {});
            }
        }

        /**
         * @brief Append @p E to @p mc's eager-set, building a fresh @ref EagerSetSnapshot.
         *
         * Spec §3.3 step 5 — only stateful eager Extensions land here; stateless ones share a
         * singleton facade and need no per-closure storage. Lazy stateful Extensions are skipped
         * too: they materialise on first query, not at construction time.
         *
         * Allocation discipline mirrors @ref PublishExtensionEntries: heap-allocate the new
         * snapshot and entry array; no EagerSet retirement queue exists yet, so the prior pointer
         * is simply dropped (small leak under idempotent install, same trade-off as the dispatch
         * path). Eager-set composition is exercised in T10 once eager + stateful fixtures arrive.
         */
        template<typename E, typename B>
        inline void AppendToEagerSet(MetaClass& mc) noexcept {
            MetaLinks* links = mc.links();
            if (links == nullptr) {
                return;  // PublishExtensionEntries always runs first and ensures links exist.
            }
            const EagerSetSnapshot* old = links->eagerSet.load(std::memory_order_acquire);
            const std::size_t oldCount = (old != nullptr) ? old->count : 0;

            auto** entries = new const MetaCore*[oldCount + 1];
            for (std::size_t i = 0; i < oldCount; ++i) {
                entries[i] = old->entries[i];
            }
            entries[oldCount] = &MetaCoreOf<E>;
            auto* fresh = new EagerSetSnapshot{oldCount + 1, entries, old};
            links->eagerSet.store(fresh, std::memory_order_release);
        }

    } // namespace Detail
    /// @endcond

    namespace Registry {

        /**
         * @brief Publish dispatch + eager-set deltas for the Extension class @p E.
         *
         * Spec §3.3 step 2 + step 5. The body walks @c Anno::Extends reflected from @p E and, for
         * each extendee @c B, composes a fresh dispatch snapshot via
         * @ref Detail::PublishExtensionEntries — replacing same-iid entries (spec §3.2 precedence
         * gives Extensions priority over Implementations) and adding one entry per iid in
         * @c implements(E). When @p E is stateful *and* @c Anno::Eager, the registrar also appends
         * @p E to @c B's @ref EagerSetSnapshot so the T10 construction hook materialises it on
         * nucleus build.
         *
         * Idempotent: the @ref AlreadyInstalled gate at the top makes the same Extension's
         * registrar a no-op on second touch (host-then-DLL or DLL-then-DLL re-run). Per-extendee
         * mutex serialisation prevents two writers from racing on the same metaclass; writers on
         * unrelated metaclasses still proceed in parallel.
         */
        template<ExtensionClass E>
        inline void Install() noexcept {
            const Iid id = IidOf<E>();
            if (AlreadyInstalled(id)) return;

            template for (constexpr auto Breflect : Detail::kExtendsInfos<E>) {
                using B = [:Breflect:];
                auto& mc = MetaClassOf<B>;
                std::lock_guard guard{WriterMutexFor(mc)};

                Detail::PublishExtensionEntries<E, B>(mc);
                if constexpr (StatefulExtensionClass<E> && Anno::IsEager<E>) {
                    Detail::AppendToEagerSet<E, B>(mc);
                }
            }
            MarkInstalled(id);
            Detail::SweepRetirements();
        }

    } // namespace Registry

} // namespace Yuki
