/**
 * @file RuntimeProvider.h
 * @brief Stable provider identity and diagnostics shared by runtime discovery frontends.
 * @ingroup Compute
 */
#pragma once

#include <compare>
#include <cstdint>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Stable identity of one backend enumeration provider, independent of its service instances. */
    struct RuntimeProviderId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this value identifies one discovery provider. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(RuntimeProviderId, RuntimeProviderId) noexcept = default;
        friend constexpr auto operator<=>(RuntimeProviderId, RuntimeProviderId) noexcept = default;
    };

    /** @brief Stable backend-independent diagnostic emitted while constructing one provider contribution. */
    struct RuntimeProviderError {
        uint64_t domain = 0;
        uint64_t code = 0;
        uint64_t detail = 0;

        friend constexpr bool operator==(RuntimeProviderError, RuntimeProviderError) noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<RuntimeProviderId>);
    static_assert(std::is_standard_layout_v<RuntimeProviderId>);
    static_assert(std::is_trivially_copyable_v<RuntimeProviderError>);
    static_assert(std::is_standard_layout_v<RuntimeProviderError>);

} // namespace Sora::Compute
