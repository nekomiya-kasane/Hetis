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

#include <Yuki/Core/MetaClass.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

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

} // namespace Yuki
