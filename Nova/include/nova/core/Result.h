#pragma once

#include <expected>
#include <cstdint>

namespace nova {

    enum class ErrorCode : uint8_t {
        None = 0,
    };

    template<typename T>
    using Result = std::expected<T, ErrorCode>;

    using VoidResult = std::expected<void, ErrorCode>;

} // namespace nova
