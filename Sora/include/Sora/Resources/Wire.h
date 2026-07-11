/**
 * @file Wire.h
 * @brief Reflection-driven little-endian serialization for Sora resource schemas.
 * @ingroup Resources
 */
#pragma once

#include <Sora/ErrorCode.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Resources::Wire {

    namespace Detail {

        template<typename T>
        consteval void ValidateSchema() {
            static_assert(std::is_trivially_copyable_v<T>, "Wire schema types must be trivially copyable.");
            template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
                using Field = typename [:std::meta::type_of(member):];
                static_assert(Sora::Concept::IntegerLike<Field>,
                              "Wire schema fields must be integral or scoped enum scalar fields.");
            }
        }

        template<Sora::Concept::IntegerLike T>
        constexpr void AppendScalar(std::vector<std::byte>& out, T value) {
            auto u = Sora::CastToUnsigned(value);
            for (size_t i = 0; i < sizeof(u); ++i) {
                out.push_back(static_cast<std::byte>((u >> (i * 8)) & 0xFFu));
            }
        }

        template<Sora::Concept::IntegerLike T>
        constexpr void WriteScalar(std::span<unsigned char> out, size_t& offset, T value) noexcept {
            auto u = Sora::CastToUnsigned(value);
            for (size_t i = 0; i < sizeof(u); ++i) {
                out[offset++] = static_cast<unsigned char>((u >> (i * 8)) & 0xFFu);
            }
        }

        template<Sora::Concept::IntegerLike T, bool IsEnum>
        struct RawScalarImpl {
            using type = T;
        };

        template<Sora::Concept::IntegerLike T>
        struct RawScalarImpl<T, true> {
            using type = std::underlying_type_t<T>;
        };

        template<Sora::Concept::IntegerLike T>
        using RawScalar = typename RawScalarImpl<T, std::is_enum_v<T>>::type;

        template<Sora::Concept::IntegerLike T>
        [[nodiscard]] constexpr auto ReadScalar(std::span<const std::byte> bytes, size_t& offset) -> Result<T> {
            using Raw = RawScalar<T>;
            using U = std::make_unsigned_t<Raw>;
            if (offset > bytes.size() || sizeof(U) > bytes.size() - offset) {
                return std::unexpected(ErrorCode::ResourceCorrupted);
            }
            U u = 0;
            for (size_t i = 0; i < sizeof(U); ++i) {
                u |= static_cast<U>(static_cast<uint8_t>(bytes[offset + i])) << (i * 8);
            }
            offset += sizeof(U);
            if constexpr (std::is_enum_v<T>) {
                return static_cast<T>(static_cast<Raw>(u));
            } else {
                return static_cast<T>(u);
            }
        }

    } // namespace Detail

    /** @brief Serialized size of a reflected wire schema. */
    template<typename T>
    [[nodiscard]] consteval size_t SizeOf() {
        Detail::ValidateSchema<T>();
        size_t size = 0;
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            using Field = typename [:std::meta::type_of(member):];
            size += sizeof(Field);
        }
        return size;
    }

    /** @brief Wire offset of reflected member @p Member in schema @p T. */
    template<typename T, std::meta::info Member>
    [[nodiscard]] consteval size_t OffsetOf() {
        Detail::ValidateSchema<T>();
        size_t offset = 0;
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            if constexpr (member == Member) {
                return offset;
            } else {
                using Field = typename [:std::meta::type_of(member):];
                offset += sizeof(Field);
            }
        }
        throw "Sora::Resources::Wire::OffsetOf: member does not belong to schema.";
    }

    /** @brief Append a reflected schema value to @p out using little-endian scalar fields. */
    template<typename T>
    constexpr void Append(std::vector<std::byte>& out, const T& value) {
        Detail::ValidateSchema<T>();
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            Detail::AppendScalar(out, value.[:member:]);
        }
    }

    /** @brief Write a reflected schema value into a caller-proven byte buffer at @p offset. */
    template<typename T>
    constexpr void WriteUnchecked(std::span<unsigned char> out, size_t offset, const T& value) noexcept {
        Detail::ValidateSchema<T>();
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            Detail::WriteScalar(out, offset, value.[:member:]);
        }
    }

    /** @brief Write a reflected schema value into a caller-proven static byte array at @p offset. */
    template<typename T, size_t N>
    constexpr void WriteUnchecked(std::array<unsigned char, N>& out, size_t offset, const T& value) noexcept {
        WriteUnchecked(std::span<unsigned char>{out}, offset, value);
    }

    /** @brief Overwrite a reflected schema value at byte offset @p offset in @p out. */
    template<typename T>
    constexpr auto WriteAt(std::vector<std::byte>& out, size_t offset, const T& value) -> VoidResult {
        std::vector<std::byte> bytes;
        bytes.reserve(SizeOf<T>());
        Append(bytes, value);
        if (offset > out.size() || bytes.size() > out.size() - offset) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        std::memcpy(out.data() + offset, bytes.data(), bytes.size());
        return {};
    }

    /** @brief Read a reflected schema value from @p bytes at @p offset. */
    template<typename T>
    [[nodiscard]] constexpr auto Read(std::span<const std::byte> bytes, size_t& offset) -> Result<T> {
        Detail::ValidateSchema<T>();
        T value{};
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            using Field = typename [:std::meta::type_of(member):];
            auto field = Detail::ReadScalar<Field>(bytes, offset);
            if (!field) {
                return std::unexpected(field.error());
            }
            value.[:member:] = *field;
        }
        return value;
    }

} // namespace Sora::Resources::Wire
