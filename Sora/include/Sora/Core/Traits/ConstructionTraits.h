/**
 * @file ConstructionTraits.h
 * @brief Type relations and forwarding for source-preserving construction.
 * @ingroup Core
 *
 * @details These facilities generalize @c std::move_if_noexcept to converting construction. They select a transfer
 * that cannot damage the source before a potentially throwing construction completes. This header defines neither
 * exception classes nor the recoverable @c ErrorCode domain.
 */
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concept {

        /**
         * @brief A source that can initialize @p Target without risking destructive partial transfer.
         *
         * @details A potentially throwing direct construction is accepted only when @p Target can instead be
         * constructed from the named source lvalue. This is the construction counterpart of @c std::move_if_noexcept
         * generalized to converting constructors.
         */
        template<typename Target, typename Source>
        concept SourcePreservingConstructible =
            std::constructible_from<Target, Source> &&
            (std::is_nothrow_constructible_v<Target, Source> || std::constructible_from<Target, Source&>);

    } // namespace Concept

    /** @brief Move @p source only when construction cannot throw; otherwise preserve it and copy. */
    template<typename Target, typename Source>
        requires Concept::SourcePreservingConstructible<Target, Source>
    [[nodiscard]] constexpr decltype(auto) ForwardForConstruction(Source& source) noexcept {
        if constexpr (std::is_nothrow_constructible_v<Target, Source>) {
            return static_cast<Source&&>(source);
        } else {
            return static_cast<Source&>(source);
        }
    }

    namespace Concept {

        /** @brief A source for which @ref Sora::ForwardForConstruction selects a non-throwing construction. */
        template<typename Target, typename Source>
        concept NothrowForwardConstructible =
            SourcePreservingConstructible<Target, Source> &&
            std::is_nothrow_constructible_v<
                Target, decltype(Sora::ForwardForConstruction<Target, Source>(std::declval<Source&>()))>;

    } // namespace Concept

} // namespace Sora