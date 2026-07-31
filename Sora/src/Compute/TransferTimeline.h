/**
 * @file TransferTimeline.h
 * @brief Shared exact transfer-engine timeline primitives for runtime planners.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Transfer.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

namespace Sora::Compute::Detail {

    /** @brief Service-scoped identity of one exclusive transfer-engine timeline. */
    struct TransferTimelineKey {
        BackendServiceId service{};
        TransferEngineId engine{};

        friend constexpr bool operator==(TransferTimelineKey, TransferTimelineKey) noexcept = default;
        friend constexpr auto operator<=>(TransferTimelineKey, TransferTimelineKey) noexcept = default;
    };

    /** @brief One half-open interval carrying planner-specific transfer provenance. */
    template<typename Provenance>
    struct TransferTimelineInterval {
        uint64_t beginNanoseconds = 0;
        uint64_t endNanoseconds = 0;
        Provenance provenance{};
    };

    /** @brief Ordered intervals committed to one exact transfer engine. */
    template<typename Provenance>
    struct TransferTimelineCalendar {
        TransferTimelineKey key{};
        std::vector<TransferTimelineInterval<Provenance>> intervals{};
    };

    template<typename Provenance>
    [[nodiscard]] constexpr bool
    TransferTimelineIntervalLess(const TransferTimelineInterval<Provenance>& left,
                                 const TransferTimelineInterval<Provenance>& right) noexcept {
        return std::tuple{left.beginNanoseconds, left.provenance} <
               std::tuple{right.beginNanoseconds, right.provenance};
    }

    template<typename Provenance>
    [[nodiscard]] const TransferTimelineCalendar<Provenance>*
    FindTransferTimelineCalendar(std::span<const TransferTimelineCalendar<Provenance>> calendars,
                                 TransferTimelineKey key) noexcept {
        const auto found = std::ranges::find(calendars, key, &TransferTimelineCalendar<Provenance>::key);
        return found == calendars.end() ? nullptr : std::addressof(*found);
    }

    /**
     * @brief Find the earliest interval that avoids both committed and speculative reservations.
     * @return Start time, or empty when @p ready + @p duration overflows.
     */
    template<typename Provenance>
    [[nodiscard]] std::optional<uint64_t>
    EarliestTransferTimelineGap(std::span<const TransferTimelineCalendar<Provenance>> committed,
                                std::span<const TransferTimelineCalendar<Provenance>> speculative,
                                TransferTimelineKey key, uint64_t ready, uint64_t duration) noexcept {
        const auto* committedCalendar = FindTransferTimelineCalendar(committed, key);
        const auto* speculativeCalendar = FindTransferTimelineCalendar(speculative, key);
        uint64_t start = ready;
        size_t committedIndex = 0;
        size_t speculativeIndex = 0;
        while ((committedCalendar != nullptr && committedIndex < committedCalendar->intervals.size()) ||
               (speculativeCalendar != nullptr && speculativeIndex < speculativeCalendar->intervals.size())) {
            const TransferTimelineInterval<Provenance>* interval = nullptr;
            if (committedCalendar == nullptr || committedIndex == committedCalendar->intervals.size()) {
                interval = std::addressof(speculativeCalendar->intervals[speculativeIndex++]);
            } else if (speculativeCalendar == nullptr || speculativeIndex == speculativeCalendar->intervals.size()) {
                interval = std::addressof(committedCalendar->intervals[committedIndex++]);
            } else if (TransferTimelineIntervalLess(committedCalendar->intervals[committedIndex],
                                                    speculativeCalendar->intervals[speculativeIndex])) {
                interval = std::addressof(committedCalendar->intervals[committedIndex++]);
            } else {
                interval = std::addressof(speculativeCalendar->intervals[speculativeIndex++]);
            }
            if (duration > std::numeric_limits<uint64_t>::max() - start) {
                return std::nullopt;
            }
            if (start + duration <= interval->beginNanoseconds) {
                return start;
            }
            start = std::max(start, interval->endNanoseconds);
        }
        return duration <= std::numeric_limits<uint64_t>::max() - start ? std::optional{start} : std::nullopt;
    }

    /** @brief Insert one reservation while preserving the canonical interval order. */
    template<typename Provenance>
    void InsertTransferTimelineInterval(std::vector<TransferTimelineCalendar<Provenance>>& calendars,
                                        TransferTimelineKey key, TransferTimelineInterval<Provenance> interval) {
        auto calendar = std::ranges::find(calendars, key, &TransferTimelineCalendar<Provenance>::key);
        if (calendar == calendars.end()) {
            calendars.push_back({.key = key});
            calendar = std::prev(calendars.end());
        }
        const auto position =
            std::ranges::lower_bound(calendar->intervals, interval, TransferTimelineIntervalLess<Provenance>);
        calendar->intervals.insert(position, std::move(interval));
    }

} // namespace Sora::Compute::Detail
