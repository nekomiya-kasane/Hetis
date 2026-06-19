/**
 * @file DispatchEntry.h
 * @brief D14 — the three-arm dispatch entry + snapshot POD.
 *
 * One @ref DispatchEntry per (interface, providing class) pair. The @ref kind discriminates
 * between Y2's three storage arms; @ref arm is interpreted per-kind:
 *   - @c InlineFacade           — pointer to the inline facade subobject inside the impl frame
 *   - @c SideTableResolver      — pointer to a resolver function `RootObject*(*)(RootObject*)`
 *   - @c CodeExtensionSingleton — pointer to the singleton stateless-extension instance
 *
 * @c seal replicates the D7 seal bits (final / unique / important) onto the entry so dispatch
 * and the cross-module Install<E> check (D7.2) can read them without re-walking annotations.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <cstddef>
#include <cstdint>

namespace Yuki {

    // Forward-declared: DispatchEntry stores `const MetaCore*` but never dereferences it.
    // All dereferences live in Query / Install consumers (T18, T20) that already include
    // MetaCore.h. Adding any inline accessor here (e.g., `const MetaCore* provider() const`
    // that touches members of *providerClass) would silently reintroduce the cycle, since
    // MetaCore.h must eventually pull in DispatchEntry.h once MetaLinks's dispatch slot
    // is typed against DispatchSnapshot.
    struct MetaCore;

    /// @brief The three Y2 dispatch arms (D14).
    enum class DispatchKind : std::uint8_t {
        InlineFacade           = 0,
        SideTableResolver      = 1,
        CodeExtensionSingleton = 2,
    };

    /**
     * @brief One entry in a class's dispatch table (D14).
     *
     * Plain aggregate; lives in rodata when published as part of a @ref DispatchSnapshot.
     * Field order is stable — the binary search in @ref LookupMergedDispatch only touches
     * @ref iid and @c seal.important, so future fields appended at the tail are non-breaking.
     */
    struct DispatchEntry {
        Iid               iid{};
        DispatchKind      kind{DispatchKind::InlineFacade};
        /// Matches A1's @c ImplementsInfo::flags convention so T18 Install / T20 Query can copy
        /// seal state with a single assignment (`e.seal = info.flags;`) instead of field-by-field.
        Detail::SealFlags seal{};
        std::uint32_t     armOffset{0};  ///< Per-kind offset: InlineFacade subobject offset within the
                                         ///< impl frame; ignored by SideTableResolver /
                                         ///< CodeExtensionSingleton (which use @ref arm directly).
        /// @brief Class-level identity of the provider (T21 introspection — @c ProviderClass /
        ///        @c RoleOf read this). Stable: a single provider class per (interface, impl) pair.
        const MetaCore*   providerClass{nullptr};
        /// @brief Per-kind payload (T23 arm wiring):
        ///   - @c InlineFacade           : per-instance facade subobject pointer (when @c armOffset==0
        ///                                 the runtime falls back to @c static_cast<I*>(node)).
        ///   - @c SideTableResolver      : function pointer @c RootObject*(*)(RootObject* node);
        ///                                 reinterpret-cast at install time and on the Query hot path.
        ///   - @c CodeExtensionSingleton : function pointer @c RootObject*(*)(); returns a singleton
        ///                                 with external-lifetime payload (Acquire/Release no-ops).
        void*             arm{nullptr};
    };

    /// @brief Iid-sorted (or insertion-order) view over a contiguous @ref DispatchEntry array.
    struct DispatchSnapshot {
        std::size_t          count{0};
        const DispatchEntry* entries{nullptr};
    };

} // namespace Yuki
