/**
 * @file Batch.h
 * @brief Allocation-free SIMD batching over one or more contiguous input ranges.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Autodiff.h>
#include <Sora/Math/Simd.h>

#include <cassert>
#include <cstddef>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Structure-of-arrays view of primal and tangent inputs for batched forward AD. */
    template<typename T>
    struct JvpInput {
        std::span<const T> primal;
        std::span<const T> tangent;
    };

    /**
     * @brief Evaluate @p function over equally-sized inputs in fixed-width SIMD batches.
     * @tparam V SIMD carrier used for each batch.
     * @details Full batches use unchecked vector loads and stores. The final incomplete batch uses partial operations,
     * so no inactive lane accesses memory. The function performs no dynamic allocation.
     */
    template<Simd::SimdVecType V, typename Output, typename Function, typename... Inputs>
        requires(!std::is_const_v<Output>) &&
                std::same_as<
                    std::remove_cvref_t<std::invoke_result_t<Function&, std::conditional_t<true, V, Inputs>...>>, V> &&
                std::is_nothrow_invocable_v<Function&, std::conditional_t<true, V, Inputs>...>
    constexpr void TransformBatch(std::span<Output> output, Function&& function,
                                  std::span<const Inputs>... inputs) noexcept {
        assert(((inputs.size() == output.size()) && ...) &&
               "TransformBatch requires equally-sized input and output ranges");

        constexpr std::size_t width = V::kSize.value;
        std::size_t offset = 0;
        for (; offset + width <= output.size(); offset += width) {
            V result = std::invoke(function, Simd::UncheckedLoad<V>(inputs.subspan(offset, width))...);
            Simd::UncheckedStore(result, output.subspan(offset, width));
        }

        if (offset != output.size()) {
            V result = std::invoke(function, Simd::PartialLoad<V>(inputs.subspan(offset))...);
            Simd::PartialStore(result, output.subspan(offset));
        }
    }

    /** @brief Transform using the native SIMD width selected for @p T by the active compilation target. */
    template<Simd::Vectorizable T, typename Function, typename... Inputs>
    constexpr void TransformBatchNative(std::span<T> output, Function&& function,
                                        std::span<const Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::NativeAbiT<T>>;
        TransformBatch<V>(output, std::forward<Function>(function), inputs...);
    }

    /**
     * @brief Evaluate a JVP over structure-of-arrays inputs using SIMD batches and masked tails.
     * @details Both output channels and every input channel must have equal lengths. No temporary allocation or AoS
     * transposition is performed.
     */
    template<Simd::SimdVecType V, typename Output, typename Function, typename... Inputs>
        requires(!std::is_const_v<Output>) &&
                std::same_as<
                    std::remove_cvref_t<std::invoke_result_t<Function&, Dual<std::conditional_t<true, V, Inputs>>...>>,
                    Dual<V>> &&
                std::is_nothrow_invocable_v<Function&, Dual<std::conditional_t<true, V, Inputs>>...>
    constexpr void TransformBatchJvp(std::span<Output> primalOutput, std::span<Output> tangentOutput,
                                     Function&& function, JvpInput<Inputs>... inputs) noexcept {
        assert(primalOutput.size() == tangentOutput.size() &&
               ((inputs.primal.size() == primalOutput.size() && inputs.tangent.size() == primalOutput.size()) && ...) &&
               "TransformBatchJvp requires equally-sized primal and tangent channels");

        constexpr std::size_t width = V::kSize.value;
        std::size_t offset = 0;
        for (; offset + width <= primalOutput.size(); offset += width) {
            Dual<V> result =
                std::invoke(function, Dual<V>{Simd::UncheckedLoad<V>(inputs.primal.subspan(offset, width)),
                                              Simd::UncheckedLoad<V>(inputs.tangent.subspan(offset, width))}...);
            Simd::UncheckedStore(result.primal, primalOutput.subspan(offset, width));
            Simd::UncheckedStore(result.tangent, tangentOutput.subspan(offset, width));
        }

        if (offset != primalOutput.size()) {
            Dual<V> result = std::invoke(function, Dual<V>{Simd::PartialLoad<V>(inputs.primal.subspan(offset)),
                                                           Simd::PartialLoad<V>(inputs.tangent.subspan(offset))}...);
            Simd::PartialStore(result.primal, primalOutput.subspan(offset));
            Simd::PartialStore(result.tangent, tangentOutput.subspan(offset));
        }
    }

    /** @brief Batched JVP using the native SIMD width selected for @p T. */
    template<Simd::Vectorizable T, typename Function, typename... Inputs>
    constexpr void TransformBatchJvpNative(std::span<T> primalOutput, std::span<T> tangentOutput, Function&& function,
                                           JvpInput<Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::NativeAbiT<T>>;
        TransformBatchJvp<V>(primalOutput, tangentOutput, std::forward<Function>(function), inputs...);
    }

} // namespace Sora::Math
