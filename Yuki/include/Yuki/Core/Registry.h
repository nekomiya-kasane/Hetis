/**
 * @file Registry.h
 * @brief D7.2 / D15 L3 / D16 — runtime install entry point with cross-module seal checks.
 *
 * `Registry::Install<T>()` materialises a class's dispatch table: walks
 * @c kImplementsArr<T>, runs the D7.2 seal checks against the live snapshot, builds a new
 * sorted @ref DispatchSnapshot + flattened @ref MergedDispatchSnapshot, atomic-publishes
 * them, bumps the local cacheEpoch, broadcasts invalidation to downstream classes, and
 * retires the old snapshots via epoch-RCU. As of A3 it additionally walks @p T's direct
 * C++ bases via reflection and appends @c &T::kMetaCore onto each Y_OBJECT base's
 * @c subclassedBy reverse-edge (D16 §5.1).
 *
 * Per-class writer mutex serialises concurrent writers; readers run lock-free against
 * the atomic snapshot pointers in @ref MetaLinks.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>

#include <meta>
#include <string_view>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Out-of-line installer kernel shared by every Install<T> instantiation.
        ///
        /// Takes a (MetaCore*, MetaLinks*) pair — i.e., a type-erased @ref MetaDynamic — so the
        /// kernel does not have to be templated. The kernel runs the seal checks, builds new
        /// snapshots from @c kImplementsArr<T> (passed in by pointer + length), publishes,
        /// retires the old snapshots through @ref RetireSnapshot, and broadcasts.
        void InstallKernel(const MetaCore* core, MetaLinks* links,
                           const ImplementsInfo* implements, std::size_t implementsCount) noexcept;

        /// @brief T23 / D16: append @p subCore to @p baseLinks->subclassedBy via copy-on-write.
        ///        Idempotent; acquires @c baseLinks->writerMu internally.
        void AppendSubclassToBase(MetaLinks* baseLinks, const MetaCore* subCore) noexcept;

        /// @brief T23 §5.3 side-channel: register the (MetaCore*, MetaLinks*) pair for a
        ///        Y_OBJECT class. Called by @c Install<T>() so the D16 arm propagator can
        ///        resolve a class identity (subclassedBy stores @c MetaCore*) back to its
        ///        runtime-mutable RCU surface (@c MetaLinks*).
        void RegisterCoreLinkPair(const MetaCore* core, MetaLinks* links) noexcept;

        /// @brief T23 §5.3 lookup: returns the @c MetaLinks paired with @p core via
        ///        @ref RegisterCoreLinkPair, or @c nullptr if @p core was never installed.
        [[nodiscard]] MetaLinks* SubclassLinksFor(const MetaCore* core) noexcept;

        /// @brief Constexpr predicate: does @p B carry a public static @c kMetaCore member?
        ///        Used to filter @c T's bases down to the ones that are Y_OBJECT classes
        ///        before driving the D16 subclassedBy edge.
        template<class B>
        consteval bool HasYObjectKMetaCore() {
            for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked())) {
                if (std::meta::is_static_member(m)
                    && std::meta::identifier_of(m) == std::string_view{"kMetaCore"}
                    && std::meta::is_public(m)) {
                    return true;
                }
            }
            return false;
        }

    } // namespace Detail
    /** @endcond */

    struct Registry {
        /// @brief Install class @p T into the runtime registry (D7.2 / D15 L3 / D16).
        ///
        /// Idempotent on the same class — a second call publishes a fresh snapshot but the
        /// seal check accepts the prior entry whose @c providerClass equals @c &T::kMetaCore
        /// as a re-install rather than a conflict.
        ///
        /// A3 addition: after the snapshot publish, walks @p T's direct C++ bases via
        /// reflection and registers @c &T::kMetaCore in each Y_OBJECT base's @c subclassedBy
        /// reverse-edge so D16-flatten can later push extension entries down through the
        /// inheritance chain. Idempotent: re-installing @p T does not duplicate the
        /// subclassedBy entry (see @ref Detail::AppendSubclassToBase).
        template<class T>
        static void Install() noexcept {
            Detail::InstallKernel(
                &T::kMetaCore,
                &Detail::gLinksFor<T>,
                Detail::kImplementsArr<T>.data(),
                Detail::kImplementsArr<T>.size());

            // T23 / D16: register T as a subclass of each direct C++ base that has Y_OBJECT.
            // Reflection drives the walk so users do not have to repeat their inheritance list.
            template for (constexpr auto baseSpec
                          : std::define_static_array(
                                std::meta::bases_of(^^T, std::meta::access_context::unchecked()))) {
                using BaseT = typename [: std::meta::type_of(baseSpec) :];
                if constexpr (Detail::HasYObjectKMetaCore<BaseT>()) {
                    Detail::AppendSubclassToBase(&Detail::gLinksFor<BaseT>, &T::kMetaCore);
                }
            }
        }
    };

} // namespace Yuki
