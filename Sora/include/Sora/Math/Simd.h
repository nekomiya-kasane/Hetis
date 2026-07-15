/**
 * @file Simd.h
 * @brief C++26 data-parallel vector and mask types.
 * @ingroup Math
 */

#pragma once

#include "Simd/Vector.h"
#include "Simd/LoadStore.h"
#include "Simd/MaskReductions.h"
#include "Simd/Reductions.h"
#include "Simd/Algorithms.h"
#include "Simd/Bit.h"
#include "Simd/Complex.h"
#include "Simd/Math.h"

namespace Sora::Simd {

    template<size_t N>
    using F16 = Sora::Math::Simd::Vector<_Float16, N>;

    template<size_t N>
    using F32 = Sora::Math::Simd::Vector<float, N>;

    template<size_t N>
    using F64 = Sora::Math::Simd::Vector<double, N>;

    template<size_t N>
    using I8 = Sora::Math::Simd::Vector<std::int8_t, N>;

    template<size_t N>
    using I16 = Sora::Math::Simd::Vector<std::int16_t, N>;

    template<size_t N>
    using I32 = Sora::Math::Simd::Vector<std::int32_t, N>;

    template<size_t N>
    using I64 = Sora::Math::Simd::Vector<std::int64_t, N>;

} // namespace Sora::Simd