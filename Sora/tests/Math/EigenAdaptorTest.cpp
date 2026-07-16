#include <Sora/Math/EigenAdaptor.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>

namespace {

    using Vec3 = Sora::Math::Vec<float, 3>;
    using Mat3 = Sora::Math::Mat<float, 3, 3>;

    static_assert(std::same_as<Vec3, Eigen::Vector3f>);
    static_assert(std::same_as<Mat3, Eigen::Matrix3f>);
    static_assert(Sora::Math::EigenColumnVector<Vec3>);
    static_assert(Sora::Math::EigenDense<Mat3>);
    static_assert(Sora::Math::EigenDense<decltype(std::declval<Vec3>() + std::declval<Vec3>())>);

} // namespace

TEST_CASE("Eigen aliases preserve expression templates and fixed-size code generation", "[Sora][Math][Eigen]") {
    const Vec3 left{1.0F, 2.0F, 3.0F};
    const Vec3 right{4.0F, 5.0F, 6.0F};
    const Vec3 result = left.cwiseProduct(right) + 2.0F * left;

    REQUIRE(result.x() == 6.0F);
    REQUIRE(result.y() == 14.0F);
    REQUIRE(result.z() == 24.0F);

    const Mat3 matrix = Mat3::Identity() * 2.0F;
    REQUIRE((matrix * left).isApprox(Vec3{2.0F, 4.0F, 6.0F}));
}

TEST_CASE("Eigen map aliases operate directly on external storage", "[Sora][Math][Eigen][Map]") {
    std::array<float, 3> storage{1.0F, 2.0F, 3.0F};
    Sora::Math::VecMap<float, 3> mapped(storage.data());
    mapped *= 2.0F;

    REQUIRE(storage == std::array<float, 3>{2.0F, 4.0F, 6.0F});

    const Sora::Math::ConstVecMap<float, 3> readOnly(storage.data());
    REQUIRE(readOnly.squaredNorm() == 56.0F);
}
