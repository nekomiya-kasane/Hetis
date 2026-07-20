/**
 * @file ValueBatch.h
 * @brief Automatic scalar and SIMD execution over homogeneous point and vector batch views.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Batch.h>
#include <Sora/Math/BatchView.h>

#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<typename ExpectedScalar, HomogeneousBatch... Batches>
        inline constexpr bool kCompatibleValueBatches =
            (std::same_as<ExpectedScalar, typename std::remove_cvref_t<Batches>::ScalarType> && ...);

        template<HomogeneousBatch First, HomogeneousBatch... Rest>
        inline constexpr bool kEqualValueBatchDimensions =
            ((std::remove_cvref_t<First>::kDimension == std::remove_cvref_t<Rest>::kDimension) && ...);

        template<typename Carrier, typename Function, typename Output, typename... Inputs>
        concept ValueKernel =
            HomogeneousBatch<Output> && (HomogeneousBatch<Inputs> && ...) &&
            std::is_nothrow_invocable_v<Function&, typename std::remove_cvref_t<Inputs>::template Rebind<Carrier>...> &&
            std::same_as<std::remove_cvref_t<std::invoke_result_t<
                             Function&, typename std::remove_cvref_t<Inputs>::template Rebind<Carrier>...>>,
                         typename std::remove_cvref_t<Output>::template Rebind<Carrier>>;

        template<Simd::SimdVecType V, CoordinateMajorBatch Batch>
        [[nodiscard]] constexpr auto LoadBatchValue(const Batch& batch, size_t offset) noexcept ->
            typename std::remove_cvref_t<Batch>::template Rebind<V> {
            using Value = typename std::remove_cvref_t<Batch>::template Rebind<V>;
            constexpr size_t dimension = std::remove_cvref_t<Batch>::kDimension;
            Value result;
            template for (constexpr size_t coordinate :
                          std::define_static_array(std::views::iota(size_t{0}, dimension))) {
                const auto channel = batch.Coordinate(coordinate).subspan(offset, V::kSize.value);
                result[coordinate] = Simd::UncheckedLoad<V>(channel);
            }
            return result;
        }

        template<Simd::SimdVecType V, CoordinateMajorBatch Batch>
        constexpr void StoreBatchValue(const Batch& batch, size_t offset,
                                       const typename std::remove_cvref_t<Batch>::template Rebind<V>& value) noexcept {
            constexpr size_t dimension = std::remove_cvref_t<Batch>::kDimension;
            template for (constexpr size_t coordinate :
                          std::define_static_array(std::views::iota(size_t{0}, dimension))) {
                auto channel = batch.Coordinate(coordinate).subspan(offset, V::kSize.value);
                Simd::UncheckedStore(value[coordinate], channel);
            }
        }

    } // namespace Detail

    /**
     * @brief Evaluate one value kernel over coordinate-major batches with an explicit SIMD carrier.
     * @tparam V SIMD carrier instantiated into the same kernel that accepts scalar mathematical values.
     * @tparam Unroll Number of independent SIMD batches issued per full loop iteration.
     * @details Inputs and output retain their affine value kinds when rebound to @p V. The batch views provide SoA
     * channels, while @p function sees only ordinary @ref Vector or @ref Point values and remains layout-independent.
     */
    template<Simd::SimdVecType V, size_t Unroll = 4, CoordinateMajorBatch Output, typename Function,
             CoordinateMajorBatch... Inputs>
        requires(!std::is_const_v<typename std::remove_cvref_t<Output>::ElementType>) &&
                Detail::kEqualValueBatchDimensions<Output, Inputs...> &&
                Detail::kCompatibleValueBatches<typename V::ValueType, Output, Inputs...> &&
                Detail::ValueKernel<V, Function, Output, Inputs...> &&
                Detail::ValueKernel<typename V::ValueType, Function, Output, Inputs...>
    constexpr void TransformBatch(Output output, Function&& function, Inputs... inputs) noexcept {
        static_assert(Unroll > 0, "TransformBatch requires a positive unroll factor");
        assert(((inputs.Size() == output.Size()) && ...) &&
               "TransformBatch requires equally-sized input and output batches");

        constexpr size_t width = V::kSize.value;
        static_assert(Unroll <= std::numeric_limits<size_t>::max() / width,
                      "TransformBatch unroll factor overflows the block width");
        constexpr size_t blockWidth = Unroll * width;
        size_t offset = 0;
        for (; output.Size() - offset >= blockWidth; offset += blockWidth) {
            template for (constexpr size_t batch : Simd::Detail::kIotaArray<Unroll, size_t>) {
                const size_t batchOffset = offset + batch * width;
                auto result = std::invoke(function, Detail::LoadBatchValue<V>(inputs, batchOffset)...);
                Detail::StoreBatchValue<V>(output, batchOffset, result);
            }
        }

        for (; output.Size() - offset >= width; offset += width) {
            auto result = std::invoke(function, Detail::LoadBatchValue<V>(inputs, offset)...);
            Detail::StoreBatchValue<V>(output, offset, result);
        }

        for (; offset < output.Size(); ++offset) {
            output.Store(offset, std::invoke(function, inputs.Load(offset)...));
        }
    }

    /**
     * @brief Evaluate one value kernel over homogeneous batches with automatic layout and carrier selection.
     * @details Coordinate-major vectorizable batches use the preferred SIMD ABI. Other layouts, or kernels that do
     * not accept SIMD-rebound values, execute the same scalar kernel without allocation or layout conversion.
     */
    template<size_t Unroll = 4, HomogeneousBatch Output, typename Function, HomogeneousBatch... Inputs>
        requires(!std::is_const_v<typename std::remove_cvref_t<Output>::ElementType>) &&
                Detail::kEqualValueBatchDimensions<Output, Inputs...> &&
                Detail::kCompatibleValueBatches<typename std::remove_cvref_t<Output>::ScalarType, Output, Inputs...> &&
                Detail::ValueKernel<typename std::remove_cvref_t<Output>::ScalarType, Function, Output, Inputs...>
    constexpr void TransformBatch(Output output, Function&& function, Inputs... inputs) noexcept {
        assert(((inputs.Size() == output.Size()) && ...) &&
               "TransformBatch requires equally-sized input and output batches");

        using Scalar = typename std::remove_cvref_t<Output>::ScalarType;
        if constexpr (CoordinateMajorBatch<Output> && (CoordinateMajorBatch<Inputs> && ...) &&
                      Simd::Vectorizable<Scalar>) {
            using V = Simd::BasicVector<Scalar, Simd::PreferredAbiT<Scalar>>;
            if constexpr (requires { TransformBatch<V, Unroll>(output, function, inputs...); }) {
                TransformBatch<V, Unroll>(output, std::forward<Function>(function), inputs...);
                return;
            }
        }

        for (size_t sample = 0; sample < output.Size(); ++sample) {
            output.Store(sample, std::invoke(function, inputs.Load(sample)...));
        }
    }

} // namespace Sora::Math
