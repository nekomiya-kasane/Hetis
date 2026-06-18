/**
 * @file MetaDynamic.h
 * @brief D18 layer 3 of 3 — per-class (const MetaCore*, MetaLinks*) pair.
 *
 * MetaDynamic is the cheap one-word-pair handle that lets instances reach both their
 * immutable metadata (@ref MetaCore, rodata) and their mutable RCU snapshot surface
 * (@ref MetaLinks) without naming their own type. It is the top layer of the three-layer
 * MetaClass (D18), companion to MetaCore (Task 7) and MetaLinks (Task 8).
 *
 * @ref Detail::gLinksFor<T> holds exactly one @ref MetaLinks per Y_OBJECT class T in
 * static storage, giving it program lifetime with no allocation.
 *
 * @ref MetaDynamicOf<T> is @c inline @c constexpr so it is constant-initialised and can
 * bind as a constant expression where needed. Both pointer arms are constant expressions:
 * @c &T::kMetaCore is a constant expression because @c kMetaCore is @c static constexpr
 * inside @c T; @c &Detail::gLinksFor<T> is a constant expression because the variable
 * template has static storage duration.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaLinks.h>

namespace Yuki {

    /**
     * @brief Per-class (const MetaCore*, MetaLinks*) handle — D18 layer 3 of 3.
     *
     * Stores a rodata pointer to the class descriptor and a mutable pointer to the
     * per-class RCU snapshot surface. Both pointers have program lifetime. The struct
     * itself is trivially copyable; instances are normally accessed via the
     * @c inline @c constexpr variable @ref MetaDynamicOf<T> rather than constructed
     * directly.
     */
    struct MetaDynamic {
        const MetaCore* core;   ///< Rodata pointer to the class descriptor (lifetime = program).
        MetaLinks*      links;  ///< Per-class mutable RCU surface (lifetime = program).
    };

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Single MetaLinks per Y_OBJECT class T — static-storage-duration global variable
        ///        template; one definition per instantiation.
        template<class T>
        inline MetaLinks gLinksFor{};

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Constant-initialised @ref MetaDynamic handle for Y_OBJECT class @p T.
     *
     * @c inline @c constexpr (not just @c inline) so the object is constant-initialised
     * and the address is a constant expression in contexts that require one. The
     * @c &T::kMetaCore arm is a constant expression because @c kMetaCore is @c static
     * @c constexpr inside @p T; the @c &Detail::gLinksFor<T> arm is a constant expression
     * because that variable template has static storage duration.
     *
     * Y_OBJECT exposes this as @c static const MetaDynamic& Meta() @c noexcept so that
     * every instance can reach its links via @c this->Meta().links->dispatch.load(...)
     * without naming its own type.
     */
    template<class T>
    inline constexpr MetaDynamic MetaDynamicOf{ &T::kMetaCore, &Detail::gLinksFor<T> };

} // namespace Yuki
