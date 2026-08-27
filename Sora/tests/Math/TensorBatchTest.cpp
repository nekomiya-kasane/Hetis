/**
 * @file TensorBatchTest.cpp
 * @brief Static-value tensor batch views, layouts, and SIMD execution tests.
 * @ingroup Testing
 */
#include <Sora/Math/Tensor.h>
#include <Sora/Math/ValueBatch.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace {

    using Sora::Math::BatchLayout;
    using Sora::Math::Tensor;
    using Sora::Math::TensorBatchView;
    using Sora::Math::TransformBatch;

    using ScalarValue = Tensor<float, 2, 3>;
    using ScalarBatch = TensorBatchView<ScalarValue, float>;
    using ConstScalarBatch = TensorBatchView<ScalarValue, const float>;

    static_assert(ScalarBatch::kElementCount == 6);
    static_assert(std::same_as<ScalarBatch::template RebindElement<float>, ScalarValue>);
    static_assert(std::same_as<ScalarBatch::template RebindElement<Sora::Simd::F32<4>>,
                               Tensor<Sora::Simd::F32<4>, 2, 3>>);
    static_assert(Sora::Math::HomogeneousBatch<ScalarBatch>);
    static_assert(Sora::Math::CoordinateMajorBatch<ScalarBatch>);
    static_assert(!Sora::Math::CoordinateMajorBatch<
                      TensorBatchView<ScalarValue, float, BatchLayout::PointMajor>>);

    TEST_CASE("concrete tensor batch views preserve logical indexing across rank one to four", "[Sora][Math][Batch]") {
        using Rank1 = Tensor<float, 3>;
        using Rank2 = Tensor<float, 2, 3>;
        using Rank3 = Tensor<float, 2, 2, 3>;
        using Rank4 = Tensor<float, 2, 2, 2, 2>;

        std::array<float, Rank1::kElementCount * 2> rank1Storage{};
        std::array<float, Rank2::kElementCount * 2> rank2Storage{};
        std::array<float, Rank3::kElementCount * 2> rank3Storage{};
        std::array<float, Rank4::kElementCount * 2> rank4Storage{};
        TensorBatchView<Rank1, float> rank1{rank1Storage.data(), 2};
        TensorBatchView<Rank2, float> rank2{rank2Storage.data(), 2};
        TensorBatchView<Rank3, float> rank3{rank3Storage.data(), 2};
        TensorBatchView<Rank4, float> rank4{rank4Storage.data(), 2};

        for (size_t element = 0; element < Rank1::kElementCount; ++element) {
            rank1[element, 0] = static_cast<float>(element + 1);
            rank1[element, 1] = static_cast<float>(element + 11);
        }
        for (size_t element = 0; element < Rank2::kElementCount; ++element) {
            rank2[element, 0] = static_cast<float>(element + 1);
            rank2[element, 1] = static_cast<float>(element + 11);
        }
        for (size_t element = 0; element < Rank3::kElementCount; ++element) {
            rank3[element, 0] = static_cast<float>(element + 1);
            rank3[element, 1] = static_cast<float>(element + 11);
        }
        for (size_t element = 0; element < Rank4::kElementCount; ++element) {
            rank4[element, 0] = static_cast<float>(element + 1);
            rank4[element, 1] = static_cast<float>(element + 11);
        }

        REQUIRE(rank1.Load(0)[2] == 3.0F);
        REQUIRE(rank2.Load(1)[5] == 16.0F);
        REQUIRE(rank3.Load(0)[5] == 6.0F);
        REQUIRE(rank4.Load(1)[15] == 26.0F);

        REQUIRE(rank2[0] == 1.0F);
        REQUIRE(rank2[1] == 11.0F);
        REQUIRE(rank2[2] == 2.0F);
    }

    TEST_CASE("coordinate-major tensor batches use SIMD and scalar-only tails", "[Sora][Math][Batch][SIMD]") {
        constexpr size_t count = 9;
        std::array<float, ScalarValue::kElementCount * count> inputStorage{};
        std::array<float, ScalarValue::kElementCount * count> outputStorage{};
        for (size_t element = 0; element < ScalarValue::kElementCount; ++element) {
            for (size_t sample = 0; sample < count; ++sample) {
                inputStorage[element * count + sample] = static_cast<float>(element + sample);
            }
        }

        const ConstScalarBatch input{inputStorage.data(), count};
        const ScalarBatch output{outputStorage.data(), count};
        TransformBatch(output, [](auto value) noexcept { return value + value; }, input);

        for (size_t element = 0; element < ScalarValue::kElementCount; ++element) {
            for (size_t sample = 0; sample < count; ++sample) {
                REQUIRE(output[element, sample] == 2.0F * input[element, sample]);
            }
        }
    }

    TEST_CASE("point-major tensor batches retain layout and use scalar fallback", "[Sora][Math][Batch][AoS]") {
        constexpr size_t count = 5;
        using Batch = TensorBatchView<ScalarValue, float, BatchLayout::PointMajor>;
        std::array<float, ScalarValue::kElementCount * count> inputStorage{};
        std::array<float, ScalarValue::kElementCount * count> outputStorage{};
        for (size_t sample = 0; sample < count; ++sample) {
            for (size_t element = 0; element < ScalarValue::kElementCount; ++element) {
                inputStorage[sample * ScalarValue::kElementCount + element] = static_cast<float>(10 * sample + element);
            }
        }

        const Batch input{inputStorage.data(), count};
        const Batch output{outputStorage.data(), count};
        TransformBatch(output, [](auto value) noexcept { return value + value; }, input);

        for (size_t sample = 0; sample < count; ++sample) {
            for (size_t element = 0; element < ScalarValue::kElementCount; ++element) {
                REQUIRE(output[element, sample] == 2.0F * input[element, sample]);
            }
        }

        REQUIRE(output[0] == 0.0F);
        REQUIRE(output[1] == 20.0F);
        REQUIRE(output[2] == 40.0F);
    }

    TEST_CASE("zero-sample batches are valid and do not dereference storage", "[Sora][Math][Batch]") {
        ScalarBatch empty{nullptr, 0};
        REQUIRE(empty.Size() == 0);
        REQUIRE(empty.ElementChannel(0).empty());
        REQUIRE(empty.ElementChannel(0).data() == nullptr);
        TransformBatch(empty, [](auto value) noexcept { return value; }, empty);
    }

    TEST_CASE("batch transforms permit exact in-place views and reject partial overlap", "[Sora][Math][Batch]") {
        constexpr size_t count = 9;
        std::array<float, ScalarValue::kElementCount * count + 1> storage{};
        for (size_t index = 0; index < ScalarValue::kElementCount * count; ++index) {
            storage[index] = static_cast<float>(index);
        }

        const ScalarBatch inPlace{storage.data(), count};
        TransformBatch(inPlace, [](auto value) noexcept { return value + value; }, inPlace);
        for (size_t element = 0; element < ScalarValue::kElementCount; ++element) {
            for (size_t sample = 0; sample < count; ++sample) {
                const size_t physicalIndex = element * count + sample;
                REQUIRE(inPlace[element, sample] == 2.0F * static_cast<float>(physicalIndex));
            }
        }

        const ScalarBatch shifted{storage.data() + 1, count};
        REQUIRE_FALSE(Sora::Math::Detail::BatchAliasingAllowed(inPlace, shifted));
    }

    TEST_CASE("tensor dual carriers rebind to SIMD dual values", "[Sora][Math][Batch][Autodiff]") {
        using DualTensor = typename ScalarBatch::template RebindElement<Sora::Math::Dual<float>>;
        using SimdDualTensor = Tensor<Sora::Math::Dual<Sora::Simd::F32<4>>, 2, 3>;
        static_assert(std::same_as<DualTensor, Tensor<Sora::Math::Dual<float>, 2, 3>>);
        static_assert(Sora::Concept::TensorValue<SimdDualTensor>);
    }

}
