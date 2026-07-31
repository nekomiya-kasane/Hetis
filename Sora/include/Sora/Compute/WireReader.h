/**
 * @file WireReader.h
 * @brief Allocation-free little-endian reader shared by canonical compute packages.
 * @ingroup Compute
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace Sora::Compute::Detail {

    /** @brief Failure to read a complete value or byte range from canonical wire data. */
    struct WireReadError {
        size_t offset = 0;

        friend constexpr bool operator==(WireReadError, WireReadError) noexcept = default;
    };

    /** @brief Bounds-checked cursor over a non-owning canonical byte sequence. */
    class WireReader {
    public:
        constexpr explicit WireReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

        /** @brief Read one unsigned integer in little-endian byte order. */
        template<std::unsigned_integral T>
        [[nodiscard]] constexpr std::expected<T, WireReadError> Read() noexcept {
            if (offset_ > bytes_.size() || bytes_.size() - offset_ < sizeof(T)) {
                return std::unexpected(WireReadError{offset_});
            }
            T value = 0;
            for (size_t index = 0; index < sizeof(T); ++index) {
                value |= static_cast<T>(std::to_integer<uint8_t>(bytes_[offset_ + index])) << (index * 8U);
            }
            offset_ += sizeof(T);
            return value;
        }

        /** @brief Read one enum through its unsigned underlying representation. */
        template<typename E>
            requires std::is_enum_v<E>
        [[nodiscard]] constexpr std::expected<E, WireReadError> Read() noexcept {
            auto value = Read<std::make_unsigned_t<std::underlying_type_t<E>>>();
            if (!value) {
                return std::unexpected(value.error());
            }
            return static_cast<E>(*value);
        }

        /** @brief Read a contiguous byte slice without copying. */
        [[nodiscard]] constexpr std::expected<std::span<const std::byte>, WireReadError>
        ReadBytes(size_t size) noexcept {
            if (offset_ > bytes_.size() || size > bytes_.size() - offset_) {
                return std::unexpected(WireReadError{offset_});
            }
            const auto result = bytes_.subspan(offset_, size);
            offset_ += size;
            return result;
        }

        /** @brief Return the current absolute byte offset. */
        [[nodiscard]] constexpr size_t Offset() const noexcept { return offset_; }

        /** @brief Return whether every source byte has been consumed. */
        [[nodiscard]] constexpr bool AtEnd() const noexcept { return offset_ == bytes_.size(); }

    private:
        std::span<const std::byte> bytes_{};
        size_t offset_ = 0;
    };

} // namespace Sora::Compute::Detail
