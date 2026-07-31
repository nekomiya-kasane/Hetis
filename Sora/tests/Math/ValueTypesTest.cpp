#include <Sora/Math/Types.h>
#include <Sora/Math/ValueBatch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cfenv>
#include <limits>
#include <type_traits>

namespace {

    template<typename T>
    concept AffinePointAddable = requires(T value) { value + value; };

    static_assert(std::is_trivially_copyable_v<Sora::Math::Vec3f>);
    static_assert(std::is_standard_layout_v<Sora::Math::Vec3f>);
    static_assert(sizeof(Sora::Math::Vec3f) == 3 * sizeof(float));
    static_assert(std::is_trivially_copyable_v<Sora::Math::Point3d>);
    static_assert(sizeof(Sora::Math::Point3d) == 3 * sizeof(double));
    static_assert(std::is_trivially_copyable_v<Sora::Math::UnitVector3f>);
    static_assert(sizeof(Sora::Math::UnitVector3f) == 3 * sizeof(float));
    static_assert(!AffinePointAddable<Sora::Math::Point3f>);
    static_assert(std::same_as<Sora::Math::Vec3f, Sora::Math::Vector<float, 3>>);
    static_assert(std::same_as<Sora::Math::Mat3f, Sora::Math::Matrix<float, 3, 3>>);

    using Float4 = Sora::Simd::F32<4>;
    static_assert(sizeof(Sora::Math::Vec<Float4, 3>) == 3 * sizeof(Float4));
    static_assert(std::is_trivially_copyable_v<Sora::Math::Vec<Float4, 3>>);

    constexpr auto kCompileTimeDirection = Sora::Math::TryNormalize(Sora::Math::Vec3f{0.0F, 3.0F, 4.0F});
    static_assert(kCompileTimeDirection.has_value());
    static_assert((*kCompileTimeDirection)[0] == 0.0F);

    constexpr Sora::Math::Vec<float, 1> kCompileTimeLine{2.0F};
    static_assert(Sora::Math::Dot(kCompileTimeLine, kCompileTimeLine) == 4.0F);
    static_assert(kCompileTimeLine == kCompileTimeLine);
    constexpr auto kCompileTimeLineDirection = Sora::Math::TryNormalize(kCompileTimeLine);
    static_assert(kCompileTimeLineDirection.has_value());
    static_assert((*kCompileTimeLineDirection)[0] == 1.0F);

    static_assert(Sora::Math::CoordinateMajorBatch<Sora::Math::PointBatchView<float, 3>>);
    static_assert(
        !Sora::Math::CoordinateMajorBatch<Sora::Math::PointBatchView<float, 3, Sora::Math::BatchLayout::PointMajor>>);

} // namespace

TEST_CASE("fixed vectors and affine points preserve their mathematical operations", "[Sora][Math][Value]") {
    constexpr Sora::Math::Vec3f first{1.0F, 2.0F, 3.0F};
    constexpr Sora::Math::Vec3f second{4.0F, -1.0F, 2.0F};
    constexpr auto sum = first + second;
    constexpr auto cross = Sora::Math::Cross(first, second);
    STATIC_REQUIRE(sum[0] == 5.0F);
    STATIC_REQUIRE(Sora::Math::Dot(first, second) == 8.0F);
    STATIC_REQUIRE(cross[0] == 7.0F);
    STATIC_REQUIRE(cross[1] == 10.0F);
    STATIC_REQUIRE(cross[2] == -9.0F);

    constexpr Sora::Math::Point3f point{2.0F, 4.0F, 8.0F};
    constexpr auto translated = point + first;
    constexpr auto displacement = translated - point;
    STATIC_REQUIRE(translated[2] == 11.0F);
    STATIC_REQUIRE(displacement == first);
}

TEST_CASE("fixed matrices preserve logical indexing across layouts and carriers", "[Sora][Math][Matrix]") {
    constexpr Sora::Math::Mat3f matrix{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 10.0F};
    constexpr Sora::Math::Vec3f vector{1.0F, 2.0F, 3.0F};
    constexpr auto product = matrix * vector;
    constexpr auto identityProduct = Sora::Math::Mat3f::Identity() * vector;
    constexpr auto transpose = Sora::Math::Transpose(matrix);

    STATIC_REQUIRE(product == Sora::Math::Vec3f{14.0F, 32.0F, 53.0F});
    STATIC_REQUIRE(identityProduct == vector);
    STATIC_REQUIRE(transpose(2, 1) == matrix(1, 2));
    STATIC_REQUIRE(matrix.Storage()[1] == 4.0F);

    using RowMajor = Sora::Math::Mat<float, 2, 3, Sora::Math::MatrixLayout::RowMajor>;
    constexpr RowMajor rowMajor{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    STATIC_REQUIRE(rowMajor(1, 2) == 6.0F);
    STATIC_REQUIRE(rowMajor.Storage()[1] == 2.0F);

    constexpr Sora::Math::Mat<float, 2, 2> columnMajorSquare{1.0F, 2.0F, 3.0F, 4.0F};
    constexpr Sora::Math::Mat<float, 2, 2, Sora::Math::MatrixLayout::RowMajor> rowMajorSquare{1.0F, 2.0F, 3.0F, 4.0F};
    STATIC_REQUIRE(columnMajorSquare == rowMajorSquare);
    STATIC_REQUIRE((columnMajorSquare + rowMajorSquare)(1, 0) == 6.0F);

    using Pack = Sora::Simd::F32<4>;
    const Sora::Math::Mat<Pack, 2, 2> simdMatrix{Pack(2.0F), Pack(1.0F), Pack(-1.0F), Pack(3.0F)};
    const Sora::Math::Vec<Pack, 2> simdVector{Pack([](int lane) { return static_cast<float>(lane + 1); }), Pack(2.0F)};
    const auto simdProduct = simdMatrix * simdVector;
    for (int lane = 0; lane < 4; ++lane) {
        const Sora::Math::Mat<float, 2, 2> scalarMatrix{2.0F, 1.0F, -1.0F, 3.0F};
        const Sora::Math::Vec2f scalarVector{static_cast<float>(lane + 1), 2.0F};
        REQUIRE(simdProduct[0][lane] == (scalarMatrix * scalarVector)[0]);
        REQUIRE(simdProduct[1][lane] == (scalarMatrix * scalarVector)[1]);
    }
}

TEST_CASE("checked normalization establishes and preserves the unit-vector invariant", "[Sora][Math][UnitVector]") {
    const auto direction = Sora::Math::TryNormalize(Sora::Math::Vec3d{3.0, 4.0, 0.0});
    REQUIRE(direction.has_value());
    REQUIRE(Sora::Math::Norm(direction->Value()) == Catch::Approx(1.0).epsilon(1.0e-14));

    const auto reversed = -*direction;
    REQUIRE(Sora::Math::Norm(reversed.Value()) == Catch::Approx(1.0).epsilon(1.0e-14));
    REQUIRE(reversed[0] == Catch::Approx(-0.6));

    const auto zero = Sora::Math::TryNormalize(Sora::Math::Vec3d{});
    REQUIRE(zero.error() == Sora::Math::NormalizationError::ZeroLength);

    const auto nonFinite =
        Sora::Math::TryNormalize(Sora::Math::Vec3d{1.0, std::numeric_limits<double>::infinity(), 0.0});
    REQUIRE(nonFinite.error() == Sora::Math::NormalizationError::NonFinite);

    const auto largest = std::numeric_limits<double>::max();
    const auto large = Sora::Math::TryNormalize(Sora::Math::Vec3d{largest, largest, 0.0});
    REQUIRE(large.has_value());
    REQUIRE(Sora::Math::Norm(large->Value()) == Catch::Approx(1.0).epsilon(1.0e-14));

    const auto smallest = std::numeric_limits<double>::denorm_min();
    const auto subnormal = Sora::Math::TryNormalize(Sora::Math::Vec3d{smallest, 0.0, 0.0});
    REQUIRE(subnormal.has_value());
    REQUIRE((*subnormal)[0] == 1.0);

    const auto nan = Sora::Math::TryNormalize(Sora::Math::Vec3d{0.0, std::numeric_limits<double>::quiet_NaN(), 1.0});
    REQUIRE(nan.error() == Sora::Math::NormalizationError::NonFinite);
}

TEST_CASE("one vector kernel has identical scalar and SIMD carrier semantics", "[Sora][Math][Value][Simd]") {
    using Pack = Sora::Simd::F32<4>;
    const Sora::Math::Vec<Pack, 3> left{Pack([](int lane) { return static_cast<float>(lane + 1); }), Pack(2.0F),
                                        Pack(3.0F)};
    const Sora::Math::Vec<Pack, 3> right{Pack(2.0F), Pack(4.0F), Pack(1.0F)};

    const Pack products = Sora::Math::Dot(left, right);
    for (int lane = 0; lane < 4; ++lane) {
        const Sora::Math::Vec3f scalarLeft{static_cast<float>(lane + 1), 2.0F, 3.0F};
        REQUIRE(products[lane] == Sora::Math::Dot(scalarLeft, Sora::Math::Vec3f{2.0F, 4.0F, 1.0F}));
    }

    const auto normalized = Sora::Math::TryNormalize(left);
    REQUIRE(normalized.has_value());
    const Pack lengths = Sora::Math::Norm(normalized->Value());
    for (int lane = 0; lane < 4; ++lane) {
        REQUIRE(lengths[lane] == Catch::Approx(1.0F).epsilon(1.0e-6F));
    }

    const Sora::Math::Vec<Pack, 3> zeroLane{Pack([](int lane) { return lane == 2 ? 0.0F : 1.0F; }), Pack(0.0F),
                                            Pack(0.0F)};
    const auto rejectedZeroLane = Sora::Math::TryNormalize(zeroLane);
    REQUIRE(rejectedZeroLane.error() == Sora::Math::NormalizationError::ZeroLength);

    const Sora::Math::Vec<Pack, 3> nonFiniteLane{
        Pack([](int lane) { return lane == 1 ? std::numeric_limits<float>::infinity() : 1.0F; }), Pack(1.0F),
        Pack(1.0F)};
    const auto rejectedNonFiniteLane = Sora::Math::TryNormalize(nonFiniteLane);
    REQUIRE(rejectedNonFiniteLane.error() == Sora::Math::NormalizationError::NonFinite);
}

TEST_CASE("coordinate-major batches instantiate the same affine kernel over SIMD packs", "[Sora][Math][Batch][SoA]") {
    constexpr size_t dimension = 3;
    constexpr size_t count = 7;
    std::array<float, dimension * count> points{};
    std::array<float, dimension * count> offsets{};
    std::array<float, dimension * count> result{};

    for (size_t coordinate = 0; coordinate < dimension; ++coordinate) {
        for (size_t sample = 0; sample < count; ++sample) {
            points[coordinate * count + sample] = static_cast<float>(10 * coordinate + sample);
            offsets[coordinate * count + sample] = static_cast<float>(coordinate + 1);
        }
    }

    const Sora::Math::PointBatchView<const float, dimension> input(points.data(), count);
    const Sora::Math::VectorBatchView<const float, dimension> offset(offsets.data(), count);
    const Sora::Math::PointBatchView<float, dimension> output(result.data(), count);
    Sora::Math::TransformBatch(output, [](auto point, auto vector) noexcept { return point + vector; }, input, offset);

    for (size_t coordinate = 0; coordinate < dimension; ++coordinate) {
        for (size_t sample = 0; sample < count; ++sample) {
            REQUIRE(output[coordinate, sample] == input[coordinate, sample] + offset[coordinate, sample]);
        }
    }
}

TEST_CASE("SIMD tails do not evaluate invented zero lanes", "[Sora][Math][Batch][SoA][Fenv]") {
    constexpr size_t count = 5;
    std::array<float, count> inputStorage{1.0F, 2.0F, 4.0F, 8.0F, 16.0F};
    std::array<float, count> outputStorage{};
    const Sora::Math::VectorBatchView<const float, 1> input(inputStorage.data(), count);
    const Sora::Math::VectorBatchView<float, 1> output(outputStorage.data(), count);

    std::feclearexcept(FE_ALL_EXCEPT);
    Sora::Math::TransformBatch(
        output,
        [](auto value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            return Value{1.0F} / value[0];
        },
        input);

    REQUIRE((std::fetestexcept(FE_DIVBYZERO | FE_INVALID) == 0));
    for (size_t sample = 0; sample < count; ++sample) {
        REQUIRE(output[0, sample] == 1.0F / input[0, sample]);
    }
}

TEST_CASE("point-major batches use the same interface without hidden transposition", "[Sora][Math][Batch][AoS]") {
    constexpr size_t dimension = 3;
    constexpr size_t count = 5;
    using InputView = Sora::Math::PointBatchView<const float, dimension, Sora::Math::BatchLayout::PointMajor>;
    using OffsetView = Sora::Math::VectorBatchView<const float, dimension, Sora::Math::BatchLayout::PointMajor>;
    using OutputView = Sora::Math::PointBatchView<float, dimension, Sora::Math::BatchLayout::PointMajor>;

    std::array<float, dimension * count> points{};
    std::array<float, dimension * count> offsets{};
    std::array<float, dimension * count> result{};
    for (size_t sample = 0; sample < count; ++sample) {
        for (size_t coordinate = 0; coordinate < dimension; ++coordinate) {
            points[sample * dimension + coordinate] = static_cast<float>(10 * sample + coordinate);
            offsets[sample * dimension + coordinate] = static_cast<float>(coordinate + 1);
        }
    }

    const InputView input(points.data(), count);
    const OffsetView offset(offsets.data(), count);
    const OutputView output(result.data(), count);
    Sora::Math::TransformBatch(output, [](auto point, auto vector) noexcept { return point + vector; }, input, offset);

    for (size_t sample = 0; sample < count; ++sample) {
        for (size_t coordinate = 0; coordinate < dimension; ++coordinate) {
            REQUIRE(output[coordinate, sample] == input[coordinate, sample] + offset[coordinate, sample]);
        }
    }
}
