/**
 * @file Types.h
 * @brief Canonical fixed-size Eigen vector and matrix aliases for Sora scalar types.
 * @ingroup Math
 */
#pragma once

#include "Sora/Core/Traits/TypeTraits.h"

#include "EigenAdaptor.h"

namespace Sora {

    namespace Math {

        using Vec2f = Vec<Traits::Float<32>, 2>;
        using Vec3f = Vec<Traits::Float<32>, 3>;
        using Vec4f = Vec<Traits::Float<32>, 4>;

        static_assert(sizeof(Vec2f) == 2 * sizeof(float), "Vec2f must be tightly packed");
        static_assert(sizeof(Vec3f) == 3 * sizeof(float), "Vec3f must be tightly packed");
        static_assert(sizeof(Vec4f) == 4 * sizeof(float), "Vec4f must be tightly packed");

        using Vec2d = Vec<Traits::Float<64>, 2>;
        using Vec3d = Vec<Traits::Float<64>, 3>;
        using Vec4d = Vec<Traits::Float<64>, 4>;

        static_assert(sizeof(Vec2d) == 2 * sizeof(double), "Vec2d must be tightly packed");
        static_assert(sizeof(Vec3d) == 3 * sizeof(double), "Vec3d must be tightly packed");
        static_assert(sizeof(Vec4d) == 4 * sizeof(double), "Vec4d must be tightly packed");

        using Vec2i = Vec<Traits::Signed<32>, 2>;
        using Vec3i = Vec<Traits::Signed<32>, 3>;
        using Vec4i = Vec<Traits::Signed<32>, 4>;

        static_assert(sizeof(Vec2i) == 2 * sizeof(int), "Vec2i must be tightly packed");
        static_assert(sizeof(Vec3i) == 3 * sizeof(int), "Vec3i must be tightly packed");
        static_assert(sizeof(Vec4i) == 4 * sizeof(int), "Vec4i must be tightly packed");

        using Vec2u = Vec<Traits::Unsigned<32>, 2>;
        using Vec3u = Vec<Traits::Unsigned<32>, 3>;
        using Vec4u = Vec<Traits::Unsigned<32>, 4>;

        static_assert(sizeof(Vec2u) == 2 * sizeof(unsigned int), "Vec2u must be tightly packed");
        static_assert(sizeof(Vec3u) == 3 * sizeof(unsigned int), "Vec3u must be tightly packed");
        static_assert(sizeof(Vec4u) == 4 * sizeof(unsigned int), "Vec4u must be tightly packed");

        using Mat2f = Mat<Traits::Float<32>, 2, 2>;
        using Mat3f = Mat<Traits::Float<32>, 3, 3>;
        using Mat4f = Mat<Traits::Float<32>, 4, 4>;

        static_assert(sizeof(Mat2f) == 4 * sizeof(float), "Mat2f must be tightly packed");
        static_assert(sizeof(Mat3f) == 9 * sizeof(float), "Mat3f must be tightly packed");
        static_assert(sizeof(Mat4f) == 16 * sizeof(float), "Mat4f must be tightly packed");

        using Mat2d = Mat<Traits::Float<64>, 2, 2>;
        using Mat3d = Mat<Traits::Float<64>, 3, 3>;
        using Mat4d = Mat<Traits::Float<64>, 4, 4>;

        static_assert(sizeof(Mat2d) == 4 * sizeof(double), "Mat2d must be tightly packed");
        static_assert(sizeof(Mat3d) == 9 * sizeof(double), "Mat3d must be tightly packed");
        static_assert(sizeof(Mat4d) == 16 * sizeof(double), "Mat4d must be tightly packed");

        using Mat2i = Mat<Traits::Signed<32>, 2, 2>;
        using Mat3i = Mat<Traits::Signed<32>, 3, 3>;
        using Mat4i = Mat<Traits::Signed<32>, 4, 4>;

        static_assert(sizeof(Mat2i) == 4 * sizeof(int), "Mat2i must be tightly packed");
        static_assert(sizeof(Mat3i) == 9 * sizeof(int), "Mat3i must be tightly packed");
        static_assert(sizeof(Mat4i) == 16 * sizeof(int), "Mat4i must be tightly packed");

        using Mat2u = Mat<Traits::Unsigned<32>, 2, 2>;
        using Mat3u = Mat<Traits::Unsigned<32>, 3, 3>;
        using Mat4u = Mat<Traits::Unsigned<32>, 4, 4>;

        static_assert(sizeof(Mat2u) == 4 * sizeof(unsigned int), "Mat2u must be tightly packed");
        static_assert(sizeof(Mat3u) == 9 * sizeof(unsigned int), "Mat3u must be tightly packed");
        static_assert(sizeof(Mat4u) == 16 * sizeof(unsigned int), "Mat4u must be tightly packed");

    } // namespace Math

} // namespace Sora
