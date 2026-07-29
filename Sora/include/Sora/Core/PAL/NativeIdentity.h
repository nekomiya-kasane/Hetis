/**
 * @file NativeIdentity.h
 * @brief Strong operating-system process and thread identifiers used for native observation and correlation.
 * @ingroup PAL
 */
#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace Sora::PAL {

    /** @brief Strong representation of an operating-system process identifier, including native value zero. */
    class NativeProcessId {
    public:
        constexpr NativeProcessId() noexcept = default;
        explicit constexpr NativeProcessId(uint64_t value) noexcept : value_{value} {}

        /** @brief Return the platform-neutral unsigned representation. */
        [[nodiscard]] constexpr uint64_t Value() const noexcept { return value_; }

        friend constexpr auto operator<=>(const NativeProcessId&, const NativeProcessId&) noexcept = default;

    private:
        uint64_t value_ = 0;
    };

    /** @brief Strong representation of an operating-system thread identifier, including native value zero. */
    class NativeThreadId {
    public:
        constexpr NativeThreadId() noexcept = default;
        explicit constexpr NativeThreadId(uint64_t value) noexcept : value_{value} {}

        /** @brief Return the platform-neutral unsigned representation. */
        [[nodiscard]] constexpr uint64_t Value() const noexcept { return value_; }

        friend constexpr auto operator<=>(const NativeThreadId&, const NativeThreadId&) noexcept = default;

    private:
        uint64_t value_ = 0;
    };

    static_assert(sizeof(NativeProcessId) == sizeof(uint64_t));
    static_assert(sizeof(NativeThreadId) == sizeof(uint64_t));

} // namespace Sora::PAL

template<>
struct std::hash<Sora::PAL::NativeProcessId> {
    [[nodiscard]] size_t operator()(Sora::PAL::NativeProcessId value) const noexcept {
        return std::hash<uint64_t>{}(value.Value());
    }
};

template<>
struct std::hash<Sora::PAL::NativeThreadId> {
    [[nodiscard]] size_t operator()(Sora::PAL::NativeThreadId value) const noexcept {
        return std::hash<uint64_t>{}(value.Value());
    }
};
