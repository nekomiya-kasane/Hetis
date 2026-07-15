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

#include <Sora/Core/Traits/TypeTraits.h>

namespace Sora::Simd {

    namespace Detail {

        template<size_t L, size_t N>
        using F = Sora::Math::Simd::Vector<Sora::Traits::Float<L>, N>;

        template<size_t L, size_t N>
        using I = Sora::Math::Simd::Vector<Sora::Traits::Signed<L>, N>;

        template<size_t L, size_t N>
        using U = Sora::Math::Simd::Vector<Sora::Traits::Unsigned<L>, N>;

    } // namespace Detail

    template<size_t N>
    using F16 = Detail::F<16, N>;

    template<size_t N>
    using F32 = Detail::F<32, N>;

    template<size_t N>
    using F64 = Detail::F<64, N>;

    template<size_t N>
    using I8 = Detail::I<8, N>;

    template<size_t N>
    using I16 = Detail::I<16, N>;

    template<size_t N>
    using I32 = Detail::I<32, N>;

    template<size_t N>
    using I64 = Detail::I<64, N>;

    template<size_t N>
    using U8 = Detail::U<8, N>;

    template<size_t N>
    using U16 = Detail::U<16, N>;

    template<size_t N>
    using U32 = Detail::U<32, N>;

    template<size_t N>
    using U64 = Detail::U<64, N>;

} // namespace Sora::Simd