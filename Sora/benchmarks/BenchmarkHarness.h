/**
 * @file BenchmarkHarness.h
 * @brief Adaptive interleaved measurement utilities for Sora microbenchmarks.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>

namespace Sora::Benchmark {

    /** @brief Non-owning, allocation-free reference to one full-batch benchmark operation. */
    struct Case {
        std::string_view name;
        void* state = nullptr;
        void (*invoke)(void*) noexcept = nullptr;

        /** @brief Invoke the referenced operation once. */
        void operator()() const noexcept { invoke(state); }
    };

    /** @brief Build a benchmark case from a callable whose lifetime encloses the measurement. */
    template<typename Function>
        requires std::is_nothrow_invocable_v<Function&>
    [[nodiscard]] Case MakeCase(std::string_view name, Function& function) noexcept {
        return {
            name,
            std::addressof(function),
            [](void* state) noexcept { std::invoke(*static_cast<Function*>(state)); },
        };
    }

    /** @brief Return the median of an odd, non-empty fixed-size sample set. */
    template<size_t SampleCount>
        requires(SampleCount > 0 && SampleCount % 2 == 1)
    [[nodiscard]] double Median(std::array<double, SampleCount> samples) noexcept {
        std::ranges::sort(samples);
        return samples[SampleCount / 2];
    }

    /** @brief Interleaved timing samples and derived paired statistics for one benchmark group. */
    template<size_t CaseCount, size_t SampleCount>
        requires(CaseCount > 0 && SampleCount > 0 && SampleCount % 2 == 1)
    struct Result {
        std::array<std::array<double, SampleCount>, CaseCount> nanosecondsPerElement{};
        size_t repetitions = 0;

        /** @brief Return the per-case median time. */
        [[nodiscard]] double MedianTime(size_t caseIndex) const noexcept {
            return Median(nanosecondsPerElement[caseIndex]);
        }

        /** @brief Return the median of paired per-sample time ratios. */
        [[nodiscard]] double PairedRatio(size_t numerator, size_t denominator) const noexcept {
            std::array<double, SampleCount> ratios{};
            for (size_t sample = 0; sample < SampleCount; ++sample) {
                ratios[sample] = nanosecondsPerElement[numerator][sample] / nanosecondsPerElement[denominator][sample];
            }
            return Median(ratios);
        }
    };

    namespace Detail {

        [[nodiscard]] inline std::chrono::steady_clock::duration MeasureDuration(const Case& benchmarkCase,
                                                                                 size_t repetitions) noexcept {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const auto begin = std::chrono::steady_clock::now();
            for (size_t repetition = 0; repetition < repetitions; ++repetition) {
                benchmarkCase();
            }
            const auto end = std::chrono::steady_clock::now();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            return end - begin;
        }

        template<size_t CaseCount>
        [[nodiscard]] size_t Calibrate(const std::array<Case, CaseCount>& cases,
                                       std::chrono::steady_clock::duration minimumSampleTime) noexcept {
            size_t repetitions = 1;
            for (;;) {
                bool allCasesLongEnough = true;
                for (const Case& benchmarkCase : cases) {
                    allCasesLongEnough &= MeasureDuration(benchmarkCase, repetitions) >= minimumSampleTime;
                }
                if (allCasesLongEnough) {
                    return repetitions;
                }
                assert(repetitions <= std::numeric_limits<size_t>::max() / 2 &&
                       "benchmark repetition calibration overflowed");
                repetitions *= 2;
            }
        }

    } // namespace Detail

    /**
     * @brief Measure cases with a common calibrated workload and alternating forward/reverse rotation.
     * @tparam SampleCount Positive odd sample count used for median statistics.
     * @param cases Operations over the same number of logical elements.
     * @param elementCount Logical elements processed by one invocation of every case.
     * @param minimumSampleTime Minimum calibrated wall time of every case in one sample.
     */
    template<size_t SampleCount, size_t CaseCount>
        requires(SampleCount > 0 && SampleCount % 2 == 1 && CaseCount > 0)
    [[nodiscard]] Result<CaseCount, SampleCount>
    MeasureInterleaved(const std::array<Case, CaseCount>& cases, size_t elementCount,
                       std::chrono::steady_clock::duration minimumSampleTime) noexcept {
        assert(elementCount > 0);
        assert(minimumSampleTime > std::chrono::steady_clock::duration::zero());
        for (const Case& benchmarkCase : cases) {
            assert(!benchmarkCase.name.empty() && benchmarkCase.invoke != nullptr);
            for (size_t warmup = 0; warmup < 3; ++warmup) {
                benchmarkCase();
            }
        }

        Result<CaseCount, SampleCount> result;
        result.repetitions = Detail::Calibrate(cases, minimumSampleTime);
        for (size_t sample = 0; sample < SampleCount; ++sample) {
            const size_t rotation = sample % CaseCount;
            const bool reverse = sample % 2 != 0;
            for (size_t position = 0; position < CaseCount; ++position) {
                const size_t caseIndex =
                    reverse ? (rotation + CaseCount - position) % CaseCount : (rotation + position) % CaseCount;
                const auto elapsed = Detail::MeasureDuration(cases[caseIndex], result.repetitions);
                result.nanosecondsPerElement[caseIndex][sample] =
                    std::chrono::duration<double, std::nano>(elapsed).count() /
                    (static_cast<double>(elementCount) * static_cast<double>(result.repetitions));
            }
        }
        return result;
    }

} // namespace Sora::Benchmark
