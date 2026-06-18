/**
 * @file Registry.h
 * @brief D7.2 / D15 L3 / D16 - runtime install entry point with cross-module seal checks.
 *
 * `Registry::Install<T>()` materialises a class's dispatch table: walks
 * `kImplementsArr<T>`, runs the D7.2 seal checks against the live snapshot, builds a new
 * sorted DispatchSnapshot + flattened MergedDispatchSnapshot, atomic-publishes them,
 * bumps the local cacheEpoch, and broadcasts invalidation to downstream classes.
 *
 * Per-nucleus writer mutex serialises concurrent Install calls on the same class;
 * readers run lock-free against the atomic snapshot pointers in MetaLinks.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {
        /// @brief Out-of-line installer kernel shared by every Install<T> instantiation.
        ///
        /// Takes a (MetaCore*, MetaLinks*) pair - i.e., a type-erased MetaDynamic - so the
        /// kernel does not have to be templated. The kernel runs the seal checks, builds
        /// new snapshots from kImplementsArr<T> (passed in by pointer + length), publishes,
        /// and broadcasts.
        void InstallKernel(const MetaCore* core, MetaLinks* links,
                           const ImplementsInfo* implements, std::size_t implementsCount) noexcept;
    } // namespace Detail
    /** @endcond */

    struct Registry {
        /// @brief Install class @p T into the runtime registry (D7.2 / D15 L3 / D16).
        ///
        /// Idempotent on the same class - a second call publishes a fresh snapshot but
        /// the seal check accepts the prior entry whose @c providerClass equals
        /// @c &T::kMetaCore as a re-install rather than a conflict.
        template<class T>
        static void Install() noexcept {
            Detail::InstallKernel(
                &T::kMetaCore,
                &Detail::gLinksFor<T>,
                Detail::kImplementsArr<T>.data(),
                Detail::kImplementsArr<T>.size());
        }
    };

} // namespace Yuki
