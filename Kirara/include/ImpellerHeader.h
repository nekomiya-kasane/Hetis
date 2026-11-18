#pragma once

#define IMPELLER_HPP_NAMESPACE
#include "impeller.hpp"

#include <Sora/Core/RefCounted.h>

namespace Ui {

    template<typename T>
    using RefCounted = Sora::RefCounted<T>;

}