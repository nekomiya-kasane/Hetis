/**
 * @file QueryL0.h
 * @brief D15 L0 — consteval Query shortcut for BOA-provided interfaces.
 *
 * L0 is the cheapest layer of the four-level Query cache: when @c Impl's @c MetaCore
 * statically lists @c I in @c kImplementsArr (a BOA / implementation-via-inheritance
 * relationship), the dispatch arm is provably an @c InlineFacade and the entire Query
 * folds to a constant @c DispatchEntry* at compile time.
 *
 * @ref Detail::IsBoaProvider returns true on that L0-eligible case. @ref Detail::L0Shortcut
 * returns a pointer to a function-local `static constexpr DispatchEntry` (program lifetime,
 * constant-initialised) when L0 applies; otherwise @c nullptr.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaCore.h>

#include <cstddef>
#include <type_traits>

namespace Yuki::Detail {

    /**
     * @brief @c true if @p Impl statically provides @p I via its @ref kImplementsArr.
     *
     * Walks @c kImplementsArr<Impl> at constant evaluation, comparing each iid against
     * @c IidOf<I>(). Returns @c false for @c Impl that is not an @c ImplementationClass
     * (Interfaces, Extensions, etc. take other arms).
     */
    template<class Impl, class I>
    consteval bool IsBoaProvider() {
        if constexpr (!::Yuki::ImplementationClass<Impl>) {
            return false;
        } else {
            for (std::size_t k = 0; k < kImplementsArr<Impl>.size(); ++k) {
                if (kImplementsArr<Impl>[k].iid == IidOf<I>()) return true;
            }
            return false;
        }
    }

    /**
     * @brief Return a program-lifetime @c DispatchEntry* for the (Impl, I) L0 fast path.
     *
     * Field @c seal is left at its in-struct default (all-false @ref SealFlags) — the L0
     * shortcut does not stamp seal bits; the runtime layers (L1 @ref FingerprintCache,
     * L2 merged-dispatch, L3 invalidation) handle sealed entries via the slower paths
     * because they need the per-(T, I) flags from @c ImplementsInfo, which the consteval
     * shortcut intentionally does not consult.
     *
     * @return Pointer to a function-local @c static @c constexpr entry when
     *         @ref IsBoaProvider returns true; @c nullptr otherwise.
     */
    template<class Impl, class I>
    constexpr const DispatchEntry* L0Shortcut() noexcept {
        if constexpr (!IsBoaProvider<Impl, I>()) {
            return nullptr;
        } else {
            static constexpr DispatchEntry kEntry{
                .iid           = IidOf<I>(),
                .kind          = DispatchKind::InlineFacade,
                .armOffset     = 0,
                .providerClass = &Impl::kMetaCore,
                .arm           = nullptr,  // resolved per-instance at Query call site.
            };
            return &kEntry;
        }
    }

} // namespace Yuki::Detail
