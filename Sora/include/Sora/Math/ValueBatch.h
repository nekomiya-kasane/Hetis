/**
 * @file ValueBatch.h
 * @brief Automatic scalar and SIMD execution over static-value batch views.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/Math/Batch.h>
#include <Sora/Math/BatchView.h>

#include <cstddef>
#include <concepts>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Traits {

        template<typename ExpectedScalar, HomogeneousBatch... Batches>
        inline constexpr bool CompatibleBatchScalars =
            (std::same_as<ExpectedScalar, typename std::remove_cvref_t<Batches>::ScalarType> && ...);

    } // namespace Traits

    namespace Concept {

        template<typename Carrier, typename Function, typename Output, typename... Inputs>
        concept ValueKernel = HomogeneousBatch<Output> && (HomogeneousBatch<Inputs> && ...) &&
                              std::invocable<
                                  Function&,
                                  typename std::remove_cvref_t<Inputs>::template RebindElement<Carrier>...> &&
                              std::same_as<std::remove_cvref_t<std::invoke_result_t<
                                               Function&, typename std::remove_cvref_t<Inputs>::template RebindElement<
                                                              Carrier>...>>,
                                           typename std::remove_cvref_t<Output>::template RebindElement<Carrier>>;

    } // namespace Concept

    namespace Detail {

        template<HomogeneousBatch Output, HomogeneousBatch Input>
        [[nodiscard]] constexpr bool BatchAliasingAllowed(const Output& output, const Input& input) noexcept {
            using OutputBatch = std::remove_cvref_t<Output>;
            using InputBatch = std::remove_cvref_t<Input>;
            using Scalar = typename OutputBatch::ScalarType;
            static_assert(std::same_as<Scalar, typename InputBatch::ScalarType>);

            if constexpr (!(requires {
                                { output.Data() } -> std::convertible_to<const Scalar*>;
                                { input.Data() } -> std::convertible_to<const Scalar*>;
                            })) {
                return true;
            }
            if (output.Size() != input.Size()) {
                return false;
            }
            if (output.Size() == 0) {
                return true;
            }
            if (output.Size() > (std::numeric_limits<size_t>::max)() / OutputBatch::kElementCount ||
                input.Size() > (std::numeric_limits<size_t>::max)() / InputBatch::kElementCount) {
                return false;
            }

            const Scalar* const outputData = output.Data();
            const Scalar* const inputData = input.Data();
            if (outputData == inputData && OutputBatch::kElementCount == InputBatch::kElementCount &&
                OutputBatch::kLayout == InputBatch::kLayout) {
                return true;
            }

            const size_t outputCount = output.Size() * OutputBatch::kElementCount;
            const size_t inputCount = input.Size() * InputBatch::kElementCount;
            const std::less<const Scalar*> less;
            return !less(outputData, inputData + inputCount) || !less(inputData, outputData + outputCount);
        }

        template<Simd::SimdVecType V, CoordinateMajorBatch Batch>
        [[nodiscard, gnu::always_inline]] constexpr auto LoadBatchValue(const Batch& batch, size_t offset) noexcept
            -> typename std::remove_cvref_t<Batch>::template RebindElement<V> {
            using Value = typename std::remove_cvref_t<Batch>::template RebindElement<V>;
            Value result;
            for (size_t element = 0; element < std::remove_cvref_t<Batch>::kElementCount; ++element) {
                result[element] = V(Simd::LoadCtorTag{}, batch.Data() + element * batch.Size() + offset);
            }
            return result;
        }

        template<Simd::SimdVecType V, CoordinateMajorBatch Batch>
        constexpr void StoreBatchValue(const Batch& batch, size_t offset,
                                       const typename std::remove_cvref_t<Batch>::template RebindElement<V>& value)
            noexcept {
            for (size_t element = 0; element < std::remove_cvref_t<Batch>::kElementCount; ++element) {
                value[element].Store(batch.Data() + element * batch.Size() + offset);
            }
        }

    } // namespace Detail

    /**
     * @brief Evaluate one value kernel with complete SIMD packs and scalar-only tails.
     * @pre Each input is either the exact output view or has storage disjoint from the output.
     */
    template<Simd::SimdVecType V, size_t Unroll = 4, CoordinateMajorBatch Output, typename Function,
             CoordinateMajorBatch... Inputs>
        requires(!std::is_const_v<typename std::remove_cvref_t<Output>::ElementType>) &&
                Traits::CompatibleBatchScalars<typename V::ValueType, Output, Inputs...> &&
                Concept::ValueKernel<V, Function, Output, Inputs...> &&
                Concept::ValueKernel<typename V::ValueType, Function, Output, Inputs...>
    constexpr void TransformBatch(Output output, Function&& function, Inputs... inputs) noexcept(
        std::is_nothrow_invocable_v<Function&, typename std::remove_cvref_t<Inputs>::template RebindElement<V>...> &&
        std::is_nothrow_invocable_v<Function&, typename std::remove_cvref_t<Inputs>::template RebindElement<
                                                   typename V::ValueType>...>) {
        static_assert(Unroll > 0, "TransformBatch requires a positive unroll factor");
        SORA_PRE(((inputs.Size() == output.Size()) && ...));
        SORA_PRE((Detail::BatchAliasingAllowed(output, inputs) && ...));
        constexpr size_t width = V::kSize.value;
        static_assert(Unroll <= std::numeric_limits<size_t>::max() / width,
                      "TransformBatch unroll factor overflows the block width");
        constexpr size_t blockWidth = Unroll * width;
        size_t offset = 0;
        for (; output.Size() - offset >= blockWidth; offset += blockWidth) {
            for (size_t batch = 0; batch < Unroll; ++batch) {
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
     * @brief Select preferred SIMD for unit-stride batches and scalar execution for all other layouts.
     * @pre Each input is either the exact output view or has storage disjoint from the output.
     */
    template<size_t Unroll = 4, HomogeneousBatch Output, typename Function, HomogeneousBatch... Inputs>
        requires(!std::is_const_v<typename std::remove_cvref_t<Output>::ElementType>) &&
                Traits::CompatibleBatchScalars<typename std::remove_cvref_t<Output>::ScalarType, Output, Inputs...> &&
                Concept::ValueKernel<typename std::remove_cvref_t<Output>::ScalarType, Function, Output, Inputs...>
    constexpr void TransformBatch(Output output, Function&& function, Inputs... inputs) {
        SORA_PRE(((inputs.Size() == output.Size()) && ...));
        SORA_PRE((Detail::BatchAliasingAllowed(output, inputs) && ...));
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
