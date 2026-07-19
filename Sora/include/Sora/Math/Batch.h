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
#include <limits>
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
     * @tparam Unroll Number of independent SIMD batches issued per full loop iteration.
     * @details Full batches use unchecked vector loads and stores. The final incomplete batch uses partial operations,
     * so no inactive lane accesses memory. The function performs no dynamic allocation.
     */
    template<Simd::SimdVecType V, size_t Unroll = 4, typename Output, typename Function, typename... Inputs>
        requires(!std::is_const_v<Output>) &&
                std::same_as<
                    std::remove_cvref_t<std::invoke_result_t<Function&, std::conditional_t<true, V, Inputs>...>>, V> &&
                std::is_nothrow_invocable_v<Function&, std::conditional_t<true, V, Inputs>...>
    constexpr void TransformBatch(std::span<Output> output, Function&& function,
                                  std::span<const Inputs>... inputs) noexcept {
        static_assert(Unroll > 0, "TransformBatch requires a positive unroll factor");
        assert(((inputs.size() == output.size()) && ...) &&
               "TransformBatch requires equally-sized input and output ranges");

        constexpr size_t width = V::kSize.value;
        static_assert(Unroll <= std::numeric_limits<size_t>::max() / width,
                      "TransformBatch unroll factor overflows the block width");
        constexpr size_t blockWidth = Unroll * width;
        size_t offset = 0;
        for (; output.size() - offset >= blockWidth; offset += blockWidth) {
            template for (constexpr size_t batch : Simd::Detail::kIotaArray<Unroll, size_t>) {
                const size_t batchOffset = offset + batch * width;
                V result = std::invoke(function, Simd::UncheckedLoad<V>(inputs.subspan(batchOffset, width))...);
                Simd::UncheckedStore(result, output.subspan(batchOffset, width));
            }
        }

        for (; output.size() - offset >= width; offset += width) {
            V result = std::invoke(function, Simd::UncheckedLoad<V>(inputs.subspan(offset, width))...);
            Simd::UncheckedStore(result, output.subspan(offset, width));
        }

        if (offset != output.size()) {
            V result = std::invoke(function, Simd::PartialLoad<V>(inputs.subspan(offset))...);
            Simd::PartialStore(result, output.subspan(offset));
        }
    }

    /**
     * @brief Transform using the preferred throughput ABI and the measured default unroll factor.
     * @details The preferred ABI is capped by @ref Simd::kDefaultPreferredVectorBytes. Use the explicit @c V overload
     * to select another compile-time width or @ref TransformBatchNative for the widest native register.
     */
    template<Simd::Vectorizable T, size_t Unroll = 4, size_t Extent = std::dynamic_extent, typename Function,
             typename... Inputs>
    constexpr void TransformBatch(std::span<T, Extent> output, Function&& function,
                                  std::span<const Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::PreferredAbiT<T>>;
        TransformBatch<V, Unroll>(std::span<T>(output), std::forward<Function>(function), inputs...);
    }

    /** @brief Transform using the widest native SIMD register selected for @p T by the active target. */
    template<Simd::Vectorizable T, size_t Unroll = 4, typename Function, typename... Inputs>
    constexpr void TransformBatchNative(std::span<T> output, Function&& function,
                                        std::span<const Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::NativeAbiT<T>>;
        TransformBatch<V, Unroll>(output, std::forward<Function>(function), inputs...);
    }

    /**
     * @brief Evaluate a JVP over structure-of-arrays inputs using SIMD batches and masked tails.
     * @tparam Unroll Number of independent SIMD batches issued per full loop iteration.
     * @details Both output channels and every input channel must have equal lengths. No temporary allocation or AoS
     * transposition is performed.
     */
    template<Simd::SimdVecType V, size_t Unroll = 4, typename Output, typename Function, typename... Inputs>
        requires(!std::is_const_v<Output>) &&
                std::same_as<
                    std::remove_cvref_t<std::invoke_result_t<Function&, Dual<std::conditional_t<true, V, Inputs>>...>>,
                    Dual<V>> &&
                std::is_nothrow_invocable_v<Function&, Dual<std::conditional_t<true, V, Inputs>>...>
    constexpr void TransformBatchJvp(std::span<Output> primalOutput, std::span<Output> tangentOutput,
                                     Function&& function, JvpInput<Inputs>... inputs) noexcept {
        static_assert(Unroll > 0, "TransformBatchJvp requires a positive unroll factor");
        assert(primalOutput.size() == tangentOutput.size() &&
               ((inputs.primal.size() == primalOutput.size() && inputs.tangent.size() == primalOutput.size()) && ...) &&
               "TransformBatchJvp requires equally-sized primal and tangent channels");

        constexpr size_t width = V::kSize.value;
        static_assert(Unroll <= std::numeric_limits<size_t>::max() / width,
                      "TransformBatchJvp unroll factor overflows the block width");
        constexpr size_t blockWidth = Unroll * width;
        size_t offset = 0;
        for (; primalOutput.size() - offset >= blockWidth; offset += blockWidth) {
            template for (constexpr size_t batch : Simd::Detail::kIotaArray<Unroll, size_t>) {
                const size_t batchOffset = offset + batch * width;
                Dual<V> result = std::invoke(
                    function, Dual<V>{Simd::UncheckedLoad<V>(inputs.primal.subspan(batchOffset, width)),
                                      Simd::UncheckedLoad<V>(inputs.tangent.subspan(batchOffset, width))}...);
                Simd::UncheckedStore(result.primal, primalOutput.subspan(batchOffset, width));
                Simd::UncheckedStore(result.tangent, tangentOutput.subspan(batchOffset, width));
            }
        }

        for (; primalOutput.size() - offset >= width; offset += width) {
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

    /**
     * @brief Batched JVP using the preferred throughput ABI and the measured default unroll factor.
     * @details Use the explicit @c V overload to select another compile-time width or @ref TransformBatchJvpNative
     * for the widest native register.
     */
    template<Simd::Vectorizable T, size_t Unroll = 4, size_t PrimalExtent = std::dynamic_extent,
             size_t TangentExtent = std::dynamic_extent, typename Function, typename... Inputs>
    constexpr void TransformBatchJvp(std::span<T, PrimalExtent> primalOutput, std::span<T, TangentExtent> tangentOutput,
                                     Function&& function, JvpInput<Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::PreferredAbiT<T>>;
        TransformBatchJvp<V, Unroll>(std::span<T>(primalOutput), std::span<T>(tangentOutput),
                                     std::forward<Function>(function), inputs...);
    }

    /** @brief Batched JVP using the widest native SIMD register selected for @p T. */
    template<Simd::Vectorizable T, size_t Unroll = 4, typename Function, typename... Inputs>
    constexpr void TransformBatchJvpNative(std::span<T> primalOutput, std::span<T> tangentOutput, Function&& function,
                                           JvpInput<Inputs>... inputs) noexcept {
        using V = Simd::BasicVector<T, Simd::NativeAbiT<T>>;
        TransformBatchJvp<V, Unroll>(primalOutput, tangentOutput, std::forward<Function>(function), inputs...);
    }

} // namespace Sora::Math
