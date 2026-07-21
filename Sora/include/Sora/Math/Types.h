/**
 * @file Types.h
 * @brief Canonical short aliases for Sora affine and fixed-size linear-algebra value types.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Math/BatchView.h>
#include <Sora/Math/Matrix.h>
#include <Sora/Math/UnitVector.h>

namespace Sora::Math {

    using Vec2f = Vec<Sora::Traits::Float<32>, 2>;
    using Vec3f = Vec<Sora::Traits::Float<32>, 3>;
    using Vec4f = Vec<Sora::Traits::Float<32>, 4>;
    using Vec2d = Vec<Sora::Traits::Float<64>, 2>;
    using Vec3d = Vec<Sora::Traits::Float<64>, 3>;
    using Vec4d = Vec<Sora::Traits::Float<64>, 4>;
    using Vec2i = Vec<Sora::Traits::Signed<32>, 2>;
    using Vec3i = Vec<Sora::Traits::Signed<32>, 3>;
    using Vec4i = Vec<Sora::Traits::Signed<32>, 4>;
    using Vec2u = Vec<Sora::Traits::Unsigned<32>, 2>;
    using Vec3u = Vec<Sora::Traits::Unsigned<32>, 3>;
    using Vec4u = Vec<Sora::Traits::Unsigned<32>, 4>;

    using Mat2f = Mat<Sora::Traits::Float<32>, 2, 2>;
    using Mat3f = Mat<Sora::Traits::Float<32>, 3, 3>;
    using Mat4f = Mat<Sora::Traits::Float<32>, 4, 4>;
    using Mat2d = Mat<Sora::Traits::Float<64>, 2, 2>;
    using Mat3d = Mat<Sora::Traits::Float<64>, 3, 3>;
    using Mat4d = Mat<Sora::Traits::Float<64>, 4, 4>;
    using Mat2i = Mat<Sora::Traits::Signed<32>, 2, 2>;
    using Mat3i = Mat<Sora::Traits::Signed<32>, 3, 3>;
    using Mat4i = Mat<Sora::Traits::Signed<32>, 4, 4>;
    using Mat2u = Mat<Sora::Traits::Unsigned<32>, 2, 2>;
    using Mat3u = Mat<Sora::Traits::Unsigned<32>, 3, 3>;
    using Mat4u = Mat<Sora::Traits::Unsigned<32>, 4, 4>;

    using Point2f = Point<Sora::Traits::Float<32>, 2>;
    using Point3f = Point<Sora::Traits::Float<32>, 3>;
    using Point4f = Point<Sora::Traits::Float<32>, 4>;
    using Point2d = Point<Sora::Traits::Float<64>, 2>;
    using Point3d = Point<Sora::Traits::Float<64>, 3>;
    using Point4d = Point<Sora::Traits::Float<64>, 4>;

    using UnitVector2f = UnitVector<Sora::Traits::Float<32>, 2>;
    using UnitVector3f = UnitVector<Sora::Traits::Float<32>, 3>;
    using UnitVector4f = UnitVector<Sora::Traits::Float<32>, 4>;
    using UnitVector2d = UnitVector<Sora::Traits::Float<64>, 2>;
    using UnitVector3d = UnitVector<Sora::Traits::Float<64>, 3>;
    using UnitVector4d = UnitVector<Sora::Traits::Float<64>, 4>;

    static_assert(sizeof(Vec3f) == 3 * sizeof(float));
    static_assert(sizeof(Vec3d) == 3 * sizeof(double));
    static_assert(sizeof(Mat3f) == 9 * sizeof(float));
    static_assert(sizeof(Mat3d) == 9 * sizeof(double));
    static_assert(sizeof(Point3f) == 3 * sizeof(float));
    static_assert(sizeof(Point3d) == 3 * sizeof(double));
    static_assert(sizeof(UnitVector3f) == 3 * sizeof(float));
    static_assert(sizeof(UnitVector3d) == 3 * sizeof(double));

} // namespace Sora::Math
