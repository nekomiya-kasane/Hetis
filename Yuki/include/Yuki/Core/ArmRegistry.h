/**
 * @file ArmRegistry.h
 * @brief T23 §6.3 — register a single arm against a nucleus (D14 SideTableResolver +
 *        CodeExtensionSingleton), then propagate via D16 to every subclass.
 *
 * Unlike @c Registry::Install<T>() (which materialises @p T's full @c kImplementsArr),
 * the arm-registration entry points add ONE @c (interface, providerClass, armKind, arm)
 * tuple to the nucleus's @c mergedDispatch, then walk @c subclassedBy and append the
 * same tuple to each subclass's @c mergedDispatch (§5.3 algorithm). Direct-subclass
 * flatten only; transitive flatten falls out naturally because subclasses' subclassedBy
 * are walked in the recursive case.
 *
 * Seal-check is intentionally elided here: extensions are inherently dynamic; the seal
 * is enforced by @c Registry::Install when the nucleus or any subclass declares an
 * @ref Anno::Implements for the same iid.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/RootObject.h>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Append (or replace by-iid) a single @ref DispatchEntry into
        ///        @p implLinks->mergedDispatch, then walk @c subclassedBy and recursively
        ///        propagate the entry into every (transitive) subclass's mergedDispatch.
        ///
        /// @param implCore     provider class — written into @c entry.providerClass.
        /// @param implLinks    nucleus class's MetaLinks; primary install target.
        /// @param entry        the entry to append. @c entry.providerClass is overwritten
        ///                     with @p implCore for hygiene.
        void RegisterArmAt(const MetaCore* implCore, MetaLinks* implLinks,
                           DispatchEntry entry) noexcept;

    }  // namespace Detail
    /** @endcond */

    /**
     * @brief Register a @ref DispatchKind::SideTableResolver arm on @p Impl for @p I.
     *
     * @tparam Impl     The nucleus class (its mergedDispatch + every subclass's
     *                  mergedDispatch will gain an entry for @c IidOf<I>()).
     * @tparam I        The interface being provided.
     * @tparam Resolver A free function @c RootObject*(*)(RootObject* node) that returns a
     *                  +1 reference to a materialised heap facade OR @c nullptr to decline
     *                  materialisation (§6.4). Resolvers MUST NOT return a 0-refcount
     *                  pointer.
     */
    template<class Impl, class I, RootObject* (*Resolver)(RootObject*)>
    void RegisterSideTable() noexcept {
        DispatchEntry e{};
        e.iid           = IidOf<I>();
        e.kind          = DispatchKind::SideTableResolver;
        e.seal          = {};
        e.armOffset     = 0;
        e.providerClass = &Impl::kMetaCore;
        e.arm           = reinterpret_cast<void*>(Resolver);
        Detail::RegisterArmAt(&Impl::kMetaCore, &Detail::gLinksFor<Impl>, e);
    }

    /**
     * @brief Register a @ref DispatchKind::CodeExtensionSingleton arm on @p Impl for @p I.
     *
     * @tparam Impl      The nucleus class.
     * @tparam I         The interface being provided.
     * @tparam Singleton A free function @c RootObject*(*)() returning a pointer to an
     *                   external-lifetime singleton. Acquire/Release are payload-sentinel
     *                   no-ops; the singleton outlives every nucleus.
     */
    template<class Impl, class I, RootObject* (*Singleton)()>
    void RegisterCodeExt() noexcept {
        DispatchEntry e{};
        e.iid           = IidOf<I>();
        e.kind          = DispatchKind::CodeExtensionSingleton;
        e.seal          = {};
        e.armOffset     = 0;
        e.providerClass = &Impl::kMetaCore;
        e.arm           = reinterpret_cast<void*>(Singleton);
        Detail::RegisterArmAt(&Impl::kMetaCore, &Detail::gLinksFor<Impl>, e);
    }

}  // namespace Yuki
