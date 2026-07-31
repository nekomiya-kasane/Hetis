/**
 * @file Cost.h
 * @brief Overflow-safe integer cost primitives shared by compute planners.
 * @ingroup Compute
 */
#pragma once

#include <cstdint>
#include <limits>

namespace Sora::Compute::Detail {

    /** @brief Add two non-negative costs with saturation. */
    [[nodiscard]] constexpr uint64_t SaturatingAdd(uint64_t first, uint64_t second) noexcept {
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return first > maximum - second ? maximum : first + second;
    }

    /** @brief Divide @p numerator by @p denominator and round toward positive infinity. */
    [[nodiscard]] constexpr uint64_t CeilDivide(uint64_t numerator, uint64_t denominator) noexcept {
        return numerator / denominator + static_cast<uint64_t>(numerator % denominator != 0);
    }

    /** @brief Convert units and a per-second rate to an overflow-safe nanosecond cost. */
    [[nodiscard]] constexpr uint64_t RateCost(uint64_t units, uint64_t unitsPerSecond) noexcept {
        constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;
        if (units == 0) {
            return 0;
        }
        if (unitsPerSecond == 0) {
            return std::numeric_limits<uint64_t>::max();
        }

        const uint64_t wholeSeconds = units / unitsPerSecond;
        if (wholeSeconds > std::numeric_limits<uint64_t>::max() / nanosecondsPerSecond) {
            return std::numeric_limits<uint64_t>::max();
        }
        const uint64_t wholeNanoseconds = wholeSeconds * nanosecondsPerSecond;
        const uint64_t remainder = units % unitsPerSecond;
        if (remainder == 0) {
            return wholeNanoseconds;
        }
        if (remainder <= std::numeric_limits<uint64_t>::max() / nanosecondsPerSecond) {
            return SaturatingAdd(wholeNanoseconds, CeilDivide(remainder * nanosecondsPerSecond, unitsPerSecond));
        }

        const uint64_t denominatorQuotient = unitsPerSecond / nanosecondsPerSecond;
        const uint64_t denominatorRemainder = unitsPerSecond % nanosecondsPerSecond;
        uint64_t lower = 1;
        uint64_t upper = nanosecondsPerSecond;
        while (lower < upper) {
            const uint64_t middle = lower + (upper - lower) / 2;
            const uint64_t representedUnits =
                middle * denominatorQuotient + middle * denominatorRemainder / nanosecondsPerSecond;
            if (representedUnits >= remainder) {
                upper = middle;
            } else {
                lower = middle + 1;
            }
        }
        return SaturatingAdd(wholeNanoseconds, lower);
    }

} // namespace Sora::Compute::Detail
