/**
 * @file SparseTest.cpp
 * @brief Canonical sparse matrix and SpMV contract tests.
 */
#include <Sora/Math/Autodiff.h>
#include <Sora/Math/Sparse.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

    using Csr = Sora::Math::CsrMatrix<double>;
    using Transpose = Sora::Math::SparseTransposeView<double, uint32_t, Sora::Math::SparseMajorAxis::Row>;
    static_assert(!std::constructible_from<Transpose, Csr&&>);
    static_assert(!std::constructible_from<Transpose, const Csr&&>);

} // namespace

TEST_CASE("sparse builder canonicalizes duplicates and converts CSR and CSC", "[Sora][Math][Sparse]") {
    Sora::Math::CoordinateMatrixBuilder<int> builder{3, 4};
    REQUIRE(builder.Add(2, 1, 4).has_value());
    REQUIRE(builder.Add(0, 3, 2).has_value());
    REQUIRE(builder.Add(0, 3, 5).has_value());
    REQUIRE(builder.Add(1, 0, 7).has_value());

    const auto csrResult = builder.Build<Sora::Math::SparseMajorAxis::Row>();
    REQUIRE(csrResult.has_value());
    const auto& csr = *csrResult;
    REQUIRE(csr.NonZeros() == 3);
    REQUIRE(csr.At(0, 3) == 7);
    REQUIRE(csr.At(1, 0) == 7);
    REQUIRE(csr.At(2, 1) == 4);
    REQUIRE(csr.At(2, 2) == 0);

    const auto cscResult = csr.To<Sora::Math::SparseMajorAxis::Column>();
    REQUIRE(cscResult.has_value());
    const auto& csc = *cscResult;
    REQUIRE(csc.At(0, 3) == 7);
    REQUIRE(csc.At(2, 1) == 4);
    REQUIRE(std::ranges::equal(csc.Offsets(), std::array<uint32_t, 5>{0, 1, 2, 2, 3}));
    REQUIRE(std::ranges::equal(csc.Indices(), std::array<uint32_t, 3>{1, 2, 0}));
    const auto csrRoundTrip = csc.To<Sora::Math::SparseMajorAxis::Row>();
    REQUIRE(csrRoundTrip.has_value());
    REQUIRE(std::ranges::equal(csrRoundTrip->Offsets(), csr.Offsets()));
    REQUIRE(std::ranges::equal(csrRoundTrip->Indices(), csr.Indices()));
    REQUIRE(std::ranges::equal(csrRoundTrip->Values(), csr.Values()));
}

TEST_CASE("sparse SpMV handles CSR, CSC, transpose, and dimension errors", "[Sora][Math][Sparse]") {
    Sora::Math::CoordinateMatrixBuilder<double> builder{2, 3};
    REQUIRE(builder.Add(0, 0, 2.0).has_value());
    REQUIRE(builder.Add(0, 2, 3.0).has_value());
    REQUIRE(builder.Add(1, 1, 4.0).has_value());
    const auto csr = *builder.Build<Sora::Math::SparseMajorAxis::Row>();
    const std::array input{1.0, 2.0, 3.0};
    const auto result = csr.SpMV(input);
    REQUIRE(result.has_value());
    REQUIRE((*result)[0] == 11.0);
    REQUIRE((*result)[1] == 8.0);
    REQUIRE_FALSE(csr.SpMV(std::span<const double>{input.data(), 2}).has_value());
    std::array<double, 2> output{};
    REQUIRE(csr.SpMV(input, std::span<double>{output}).has_value());
    REQUIRE(output[0] == 11.0);
    REQUIRE(output[1] == 8.0);
    REQUIRE(csr.SpMV(input, std::span<double>{output.data(), 1}).error() ==
            Sora::Math::SparseError::DimensionMismatch);

    std::array<double, 3> aliased{1.0, 2.0, 3.0};
    REQUIRE(csr.SpMV(std::span<const double>{aliased}, std::span<double>{aliased.data(), 2}).error() ==
            Sora::Math::SparseError::OverlappingBuffers);

    const Sora::Math::SparseTransposeView transpose{csr};
    REQUIRE(transpose.Rows() == 3);
    REQUIRE(transpose.Columns() == 2);
    REQUIRE(transpose.At(2, 0) == 3.0);
}

TEST_CASE("sparse builder rejects out-of-range coordinates", "[Sora][Math][Sparse]") {
    Sora::Math::CoordinateMatrixBuilder<float> builder{2, 2};
    REQUIRE(builder.Add(2, 0, 1.0f).error() == Sora::Math::SparseError::IndexOutOfRange);
    REQUIRE(builder.Add(0, 2, 1.0f).error() == Sora::Math::SparseError::IndexOutOfRange);
}

TEST_CASE("sparse dimensions and explicit zeros have deterministic contracts", "[Sora][Math][Sparse]") {
    Sora::Math::CoordinateMatrixBuilder<int> empty{0, 3};
    REQUIRE(empty.Build<Sora::Math::SparseMajorAxis::Row>().error() == Sora::Math::SparseError::InvalidDimensions);

    Sora::Math::CoordinateMatrixBuilder<int> builder{1, 1};
    REQUIRE(builder.Add(0, 0, 0).has_value());
    const auto matrix = *builder.Build<Sora::Math::SparseMajorAxis::Row>();
    REQUIRE(matrix.NonZeros() == 1);
    REQUIRE(matrix.Values()[0] == 0);

    Sora::Math::CoordinateMatrixBuilder<int, uint8_t> oversizedMajor{256, 1};
    REQUIRE(oversizedMajor.Build<Sora::Math::SparseMajorAxis::Row>().error() ==
            Sora::Math::SparseError::IndexOverflow);
    Sora::Math::CoordinateMatrixBuilder<int, uint8_t> oversizedMinor{1, 257};
    REQUIRE(oversizedMinor.Add(0, 256, 1).has_value());
    REQUIRE(oversizedMinor.Build<Sora::Math::SparseMajorAxis::Row>().error() ==
            Sora::Math::SparseError::IndexOverflow);
    Sora::Math::CoordinateMatrixBuilder<int, size_t> maximumMajor{(std::numeric_limits<size_t>::max)(), 1};
    REQUIRE(maximumMajor.Build<Sora::Math::SparseMajorAxis::Row>().error() ==
            Sora::Math::SparseError::IndexOverflow);
}

TEST_CASE("sparse transpose view supports zero-copy SpMV", "[Sora][Math][Sparse]") {
    Sora::Math::CoordinateMatrixBuilder<double> builder{2, 3};
    REQUIRE(builder.Add(0, 0, 2.0).has_value());
    REQUIRE(builder.Add(0, 2, 3.0).has_value());
    REQUIRE(builder.Add(1, 1, 4.0).has_value());
    const auto matrix = *builder.Build<Sora::Math::SparseMajorAxis::Row>();
    const Sora::Math::SparseTransposeView transpose{matrix};
    const std::array input{5.0, 7.0};
    const auto output = transpose.SpMV(input);
    REQUIRE(output.has_value());
    REQUIRE((*output)[0] == 10.0);
    REQUIRE((*output)[1] == 28.0);
    REQUIRE((*output)[2] == 15.0);
    REQUIRE(transpose.Source().NonZeros() == matrix.NonZeros());
    std::array<double, 3> outputStorage{};
    REQUIRE(transpose.SpMV(input, std::span<double>{outputStorage}).has_value());
    REQUIRE(outputStorage[0] == 10.0);
    REQUIRE(outputStorage[1] == 28.0);
    REQUIRE(outputStorage[2] == 15.0);

    std::array<double, 3> aliased{5.0, 7.0, 0.0};
    REQUIRE(transpose.SpMV(std::span<const double>{aliased.data(), 2}, std::span<double>{aliased}).error() ==
            Sora::Math::SparseError::OverlappingBuffers);

    const auto cscMatrix = *builder.Build<Sora::Math::SparseMajorAxis::Column>();
    const Sora::Math::SparseTransposeView cscTranspose{cscMatrix};
    const auto cscOutput = cscTranspose.SpMV(input);
    REQUIRE(cscOutput.has_value());
    REQUIRE((*cscOutput)[0] == 10.0);
    REQUIRE((*cscOutput)[1] == 28.0);
    REQUIRE((*cscOutput)[2] == 15.0);
    REQUIRE(cscTranspose.Source().NonZeros() == cscMatrix.NonZeros());
}

TEST_CASE("sparse SpMV accepts Dual value carriers", "[Sora][Math][Sparse][Autodiff]") {
    using Dual = Sora::Math::Dual<float>;
    Sora::Math::CoordinateMatrixBuilder<Dual> builder{2, 2};
    REQUIRE(builder.Add(0, 0, Dual{2.0F, 0.0F}).has_value());
    REQUIRE(builder.Add(1, 1, Dual{3.0F, 0.0F}).has_value());
    const auto matrix = *builder.Build<Sora::Math::SparseMajorAxis::Row>();
    const std::array input{Dual{4.0F, 1.0F}, Dual{5.0F, 1.0F}};
    const auto output = matrix.SpMV(input);
    REQUIRE(output.has_value());
    REQUIRE((*output)[0].primal == 8.0F);
    REQUIRE((*output)[0].tangent == 2.0F);
    REQUIRE((*output)[1].primal == 15.0F);
    REQUIRE((*output)[1].tangent == 3.0F);
}
