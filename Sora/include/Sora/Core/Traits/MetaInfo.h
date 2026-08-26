/**
 * @file MetaInfo.h
 * @brief Fundamental C++26 reflection projections.
 * @ingroup Core
 *
 * @details This header owns only the bridge from a reflected type entity to a
 * C++ type. It is intentionally independent from TypeList, error propagation,
 * and higher-order algorithms so those facilities can depend on the same
 * primitive without creating a semantic cycle.
 */
#pragma once

#include <meta>

namespace Sora::Meta {

    /**
     * @brief Splice a reflected type entity into a C++ type.
     *
     * @tparam Info Reflection of a type entity.
     * @code{.cpp}
     * using Integer = Sora::Meta::InfoType<^^int>;
     * static_assert(std::same_as<Integer, int>);
     * @endcode
     */
    template<std::meta::info Info>
    using InfoType = [:Info:];

} // namespace Sora::Meta
