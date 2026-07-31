#include <Sora/Math/EigenAdaptor.h>
#include <Sora/Math/Types.h>

#include <Eigen/LU>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <type_traits>

#if defined(SORA_EIGEN_MKL_ENABLED) && !defined(EIGEN_USE_MKL_ALL)
#    error "The eigen target must enable its configured oneMKL backend transitively."
#endif

#if defined(SORA_EIGEN_MKL_ENABLED) && !defined(EIGEN_USE_MKL)
#    error "Eigen rejected the configured oneMKL headers or version."
#endif

#if defined(SORA_EIGEN_MKL_ENABLED) && !defined(MKL_DIRECT_CALL)
#    error "Eigen's oneMKL direct-call path must remain enabled."
#endif

namespace {

    namespace EigenInterop = Sora::Math::Interop::Eigen;

    using EigenVec3 = EigenInterop::Vector<float, 3>;
    using EigenMat3 = EigenInterop::Matrix<float, 3, 3>;

    static_assert(!std::same_as<Sora::Math::Vec3f, ::Eigen::Vector3f>);
    static_assert(std::same_as<Sora::Math::Vec3f, Sora::Math::Vector<float, 3>>);
    static_assert(EigenInterop::ColumnVector<EigenVec3>);
    static_assert(EigenInterop::Dense<EigenMat3>);
    static_assert(EigenInterop::Dense<decltype(std::declval<EigenVec3>() + std::declval<EigenVec3>())>);
#ifdef SORA_EIGEN_MKL_ENABLED
    static_assert(sizeof(MKL_INT) == sizeof(int), "Eigen requires oneMKL's LP64 interface.");
#endif

} // namespace

TEST_CASE("Eigen-owned values remain explicit backend objects", "[Sora][Math][Eigen]") {
    const EigenVec3 left{1.0F, 2.0F, 3.0F};
    const EigenVec3 right{4.0F, 5.0F, 6.0F};
    const EigenVec3 result = left.cwiseProduct(right) + 2.0F * left;

    REQUIRE(result.x() == 6.0F);
    REQUIRE(result.y() == 14.0F);
    REQUIRE(result.z() == 24.0F);

    const EigenMat3 matrix = EigenMat3::Identity() * 2.0F;
    REQUIRE((matrix * left).isApprox(EigenVec3{2.0F, 4.0F, 6.0F}));
}

TEST_CASE("canonical vectors map to Eigen without copies", "[Sora][Math][Eigen][Interop]") {
    Sora::Math::Vec3f value{1.0F, 2.0F, 3.0F};
    auto mapped = EigenInterop::AsEigen(value);
    mapped *= 2.0F;

    REQUIRE(value == Sora::Math::Vec3f{2.0F, 4.0F, 6.0F});

    const Sora::Math::Vec3f& readOnlyValue = value;
    const auto readOnly = EigenInterop::AsEigen(readOnlyValue);
    REQUIRE(readOnly.squaredNorm() == 56.0F);
}

TEST_CASE("canonical matrix layout maps directly to matching Eigen storage", "[Sora][Math][Eigen][Interop][Matrix]") {
    Sora::Math::Mat3f columnMajor{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F};
    auto columnMap = EigenInterop::AsEigen(columnMajor);
    REQUIRE(columnMap(1, 2) == 6.0F);
    columnMap(1, 2) = 12.0F;
    REQUIRE(columnMajor(1, 2) == 12.0F);

    using RowMajor = Sora::Math::Mat<float, 2, 3, Sora::Math::MatrixLayout::RowMajor>;
    RowMajor rowMajor{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    auto rowMap = EigenInterop::AsEigen(rowMajor);
    REQUIRE(rowMap(1, 2) == 6.0F);
    REQUIRE(rowMap.data() == rowMajor.Data());
}

TEST_CASE("Eigen map aliases operate directly on external storage", "[Sora][Math][Eigen][Map]") {
    std::array<float, 3> storage{1.0F, 2.0F, 3.0F};
    EigenInterop::VectorMap<float, 3> mapped(storage.data());
    mapped *= 2.0F;

    REQUIRE(storage == std::array<float, 3>{2.0F, 4.0F, 6.0F});

    const EigenInterop::ConstVectorMap<float, 3> readOnly(storage.data());
    REQUIRE(readOnly.squaredNorm() == 56.0F);
}

#ifdef SORA_EIGEN_MKL_ENABLED
TEST_CASE("Eigen dispatches large dense operations through oneMKL", "[Sora][Math][Eigen][MKL]") {
    MKLVersion version{};
    mkl_get_version(&version);
    REQUIRE(version.MajorVersion > 0);

    constexpr ::Eigen::Index size = 192;
    const ::Eigen::MatrixXd left = ::Eigen::MatrixXd::Random(size, size);
    const ::Eigen::MatrixXd right = ::Eigen::MatrixXd::Random(size, size);
    const ::Eigen::MatrixXd product = left * right;
    REQUIRE(product(17, 31) == Catch::Approx(left.row(17).dot(right.col(31))).epsilon(1.0e-12));

    ::Eigen::MatrixXd system = ::Eigen::MatrixXd::Random(size, size);
    system.diagonal().array() += static_cast<double>(size);
    const ::Eigen::VectorXd expected = ::Eigen::VectorXd::Random(size);
    const ::Eigen::VectorXd solution = system.partialPivLu().solve(system * expected);
    REQUIRE(solution.isApprox(expected, 1.0e-10));

    const ::Eigen::VectorXd input = ::Eigen::VectorXd::LinSpaced(256, -2.0, 2.0);
    const ::Eigen::VectorXd output = input.array().exp();
    REQUIRE(output[0] == Catch::Approx(std::exp(input[0])).epsilon(1.0e-12));
    REQUIRE(output[input.size() - 1] == Catch::Approx(std::exp(input[input.size() - 1])).epsilon(1.0e-12));
}
#endif
