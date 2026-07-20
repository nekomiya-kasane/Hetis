/**
 * @file Types.h
 * @brief Canonical affine value types and Eigen linear-algebra aliases for Sora scalar types.
 *
 * @ingroup Math
 */
#pragma once

#include "Sora/Core/Traits/TypeTraits.h"

#include "BatchView.h"
#include "EigenAdaptor.h"
#include "UnitVector.h"

namespace Sora {

    namespace Math {

        using Vector2f = Vector<Sora::Traits::Float<32>, 2>;
        using Vector3f = Vector<Sora::Traits::Float<32>, 3>;
        using Vector4f = Vector<Sora::Traits::Float<32>, 4>;
        using Vector2d = Vector<Sora::Traits::Float<64>, 2>;
        using Vector3d = Vector<Sora::Traits::Float<64>, 3>;
        using Vector4d = Vector<Sora::Traits::Float<64>, 4>;

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

        static_assert(sizeof(Vector2f) == 2 * sizeof(float));
        static_assert(sizeof(Vector3f) == 3 * sizeof(float));
        static_assert(sizeof(Vector4f) == 4 * sizeof(float));
        static_assert(sizeof(Vector2d) == 2 * sizeof(double));
        static_assert(sizeof(Vector3d) == 3 * sizeof(double));
        static_assert(sizeof(Vector4d) == 4 * sizeof(double));
        static_assert(sizeof(Point3f) == 3 * sizeof(float));
        static_assert(sizeof(Point3d) == 3 * sizeof(double));
        static_assert(sizeof(UnitVector3f) == 3 * sizeof(float));
        static_assert(sizeof(UnitVector3d) == 3 * sizeof(double));

        using Vec2f = Vec<Sora::Traits::Float<32>, 2>;
        using Vec3f = Vec<Sora::Traits::Float<32>, 3>;
        using Vec4f = Vec<Sora::Traits::Float<32>, 4>;

        static_assert(sizeof(Vec2f) == 2 * sizeof(float), "Vec2f must be tightly packed");
        static_assert(sizeof(Vec3f) == 3 * sizeof(float), "Vec3f must be tightly packed");
        static_assert(sizeof(Vec4f) == 4 * sizeof(float), "Vec4f must be tightly packed");

        using Vec2d = Vec<Sora::Traits::Float<64>, 2>;
        using Vec3d = Vec<Sora::Traits::Float<64>, 3>;
        using Vec4d = Vec<Sora::Traits::Float<64>, 4>;

        static_assert(sizeof(Vec2d) == 2 * sizeof(double), "Vec2d must be tightly packed");
        static_assert(sizeof(Vec3d) == 3 * sizeof(double), "Vec3d must be tightly packed");
        static_assert(sizeof(Vec4d) == 4 * sizeof(double), "Vec4d must be tightly packed");

        using Vec2i = Vec<Sora::Traits::Signed<32>, 2>;
        using Vec3i = Vec<Sora::Traits::Signed<32>, 3>;
        using Vec4i = Vec<Sora::Traits::Signed<32>, 4>;

        static_assert(sizeof(Vec2i) == 2 * sizeof(int), "Vec2i must be tightly packed");
        static_assert(sizeof(Vec3i) == 3 * sizeof(int), "Vec3i must be tightly packed");
        static_assert(sizeof(Vec4i) == 4 * sizeof(int), "Vec4i must be tightly packed");

        using Vec2u = Vec<Sora::Traits::Unsigned<32>, 2>;
        using Vec3u = Vec<Sora::Traits::Unsigned<32>, 3>;
        using Vec4u = Vec<Sora::Traits::Unsigned<32>, 4>;

        static_assert(sizeof(Vec2u) == 2 * sizeof(unsigned int), "Vec2u must be tightly packed");
        static_assert(sizeof(Vec3u) == 3 * sizeof(unsigned int), "Vec3u must be tightly packed");
        static_assert(sizeof(Vec4u) == 4 * sizeof(unsigned int), "Vec4u must be tightly packed");

        using Mat2f = Mat<Sora::Traits::Float<32>, 2, 2>;
        using Mat3f = Mat<Sora::Traits::Float<32>, 3, 3>;
        using Mat4f = Mat<Sora::Traits::Float<32>, 4, 4>;

        static_assert(sizeof(Mat2f) == 4 * sizeof(float), "Mat2f must be tightly packed");
        static_assert(sizeof(Mat3f) == 9 * sizeof(float), "Mat3f must be tightly packed");
        static_assert(sizeof(Mat4f) == 16 * sizeof(float), "Mat4f must be tightly packed");

        using Mat2d = Mat<Sora::Traits::Float<64>, 2, 2>;
        using Mat3d = Mat<Sora::Traits::Float<64>, 3, 3>;
        using Mat4d = Mat<Sora::Traits::Float<64>, 4, 4>;

        static_assert(sizeof(Mat2d) == 4 * sizeof(double), "Mat2d must be tightly packed");
        static_assert(sizeof(Mat3d) == 9 * sizeof(double), "Mat3d must be tightly packed");
        static_assert(sizeof(Mat4d) == 16 * sizeof(double), "Mat4d must be tightly packed");

        using Mat2i = Mat<Sora::Traits::Signed<32>, 2, 2>;
        using Mat3i = Mat<Sora::Traits::Signed<32>, 3, 3>;
        using Mat4i = Mat<Sora::Traits::Signed<32>, 4, 4>;

        static_assert(sizeof(Mat2i) == 4 * sizeof(int), "Mat2i must be tightly packed");
        static_assert(sizeof(Mat3i) == 9 * sizeof(int), "Mat3i must be tightly packed");
        static_assert(sizeof(Mat4i) == 16 * sizeof(int), "Mat4i must be tightly packed");

        using Mat2u = Mat<Sora::Traits::Unsigned<32>, 2, 2>;
        using Mat3u = Mat<Sora::Traits::Unsigned<32>, 3, 3>;
        using Mat4u = Mat<Sora::Traits::Unsigned<32>, 4, 4>;

        static_assert(sizeof(Mat2u) == 4 * sizeof(unsigned int), "Mat2u must be tightly packed");
        static_assert(sizeof(Mat3u) == 9 * sizeof(unsigned int), "Mat3u must be tightly packed");
        static_assert(sizeof(Mat4u) == 16 * sizeof(unsigned int), "Mat4u must be tightly packed");

    } // namespace Math

} // namespace Sora
