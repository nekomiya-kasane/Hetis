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
 * @c important / @c unique / @c final_ replicate the D7 seal bits onto the entry so dispatch
 * and the cross-module Install<E> check (D7.2) can read them without re-walking annotations.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <cstddef>
#include <cstdint>

namespace Yuki {

    struct MetaCore;  // forward-decl avoids include cycle with MetaCore.h.

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
     * @ref iid and @ref important, so future fields appended at the tail are non-breaking.
     */
    struct DispatchEntry {
        Iid              iid{};
        DispatchKind     kind{DispatchKind::InlineFacade};
        bool             important{false};
        bool             unique{false};
        bool             final_{false};
        std::uint32_t    armOffset{0};
        const MetaCore*  providerClass{nullptr};
        void*            arm{nullptr};
    };

    /// @brief Iid-sorted (or insertion-order) view over a contiguous @ref DispatchEntry array.
    struct DispatchSnapshot {
        std::size_t          count{0};
        const DispatchEntry* entries{nullptr};
    };

} // namespace Yuki
