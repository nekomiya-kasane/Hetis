/**
 * @file Wire.h
 * @brief Endian-stable byte-stream and reflected schema serialization helpers.
 * @ingroup Core
 */
#pragma once

#include <Sora/ErrorCode.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Wire {

    namespace Concept {

        /** @brief Integer or scoped-enum scalar accepted by stable wire encoders. */
        template<typename T>
        concept Scalar = Sora::Concept::IntegerLike<std::remove_cv_t<T>> && !std::same_as<std::remove_cv_t<T>, bool>;

        /** @brief Mutable byte type accepted by stable wire encoders. */
        template<typename T>
        concept MutableByte = Sora::Concept::ByteLike<T> && !std::is_const_v<T>;

    } // namespace Concept

    namespace Detail {

        template<Concept::Scalar T, bool IsEnum>
        struct RawScalarImpl {
            using type = std::remove_cv_t<T>;
        };

        template<Concept::Scalar T>
        struct RawScalarImpl<T, true> {
            using type = std::underlying_type_t<std::remove_cv_t<T>>;
        };

        template<Concept::Scalar T>
        using RawScalar = typename RawScalarImpl<T, std::is_enum_v<std::remove_cv_t<T>>>::type;

        template<Concept::Scalar T>
        using UnsignedScalar = std::make_unsigned_t<RawScalar<T>>;

        template<Concept::Scalar T>
        [[nodiscard]] constexpr T FromUnsigned(UnsignedScalar<T> value) noexcept {
            using Raw = RawScalar<T>;
            if constexpr (std::is_enum_v<std::remove_cv_t<T>>) {
                return static_cast<T>(static_cast<Raw>(value));
            } else {
                return static_cast<T>(static_cast<Raw>(value));
            }
        }

        template<Concept::MutableByte Byte>
        constexpr void StoreByte(std::span<Byte> out, size_t& offset, uint8_t value) noexcept {
            if constexpr (std::same_as<std::remove_cv_t<Byte>, std::byte>) {
                out[offset++] = static_cast<std::byte>(value);
            } else {
                out[offset++] = static_cast<Byte>(value);
            }
        }

        template<Concept::Scalar T>
        constexpr void AppendLittleEndianScalar(std::vector<std::byte>& out, T value) {
            auto u = static_cast<UnsignedScalar<T>>(Sora::CastToUnsigned(value));
            for (size_t i = 0; i < sizeof(u); ++i) {
                out.push_back(static_cast<std::byte>((u >> (i * 8u)) & 0xFFu));
            }
        }

        template<Concept::Scalar T>
        constexpr void AppendBigEndianScalar(std::vector<std::byte>& out, T value) {
            auto u = static_cast<UnsignedScalar<T>>(Sora::CastToUnsigned(value));
            for (size_t i = 0; i < sizeof(u); ++i) {
                const size_t shift = (sizeof(u) - 1u - i) * 8u;
                out.push_back(static_cast<std::byte>((u >> shift) & 0xFFu));
            }
        }

        template<typename T>
        consteval void ValidateSchema() {
            static_assert(std::is_trivially_copyable_v<T>, "Wire schema types must be trivially copyable.");
            template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
                using Field = typename [:std::meta::type_of(member):];
                static_assert(Concept::Scalar<Field>,
                              "Wire schema fields must be integral or scoped enum scalar fields.");
            }
        }

        /** @brief Return true when byte range @c [offset, offset + size) lies inside a byte image of @p extent bytes.
         */
        [[nodiscard]] constexpr bool HasRange(uint64_t extent, uint64_t offset, uint64_t size) noexcept {
            return offset <= extent && size <= extent - offset;
        }

        /** @brief Return true when byte range @c [offset, offset + size) lies inside @p bytes. */
        [[nodiscard]] constexpr bool HasRange(std::span<const std::byte> bytes, uint64_t offset,
                                              uint64_t size) noexcept {
            return HasRange(static_cast<uint64_t>(bytes.size()), offset, size);
        }

        /** @brief Return a checked byte subspan, or @ref ErrorCode::DataTruncated when the range is outside @p bytes.
         */
        [[nodiscard]] constexpr auto Subspan(std::span<const std::byte> bytes, uint64_t offset, uint64_t size)
            -> Result<std::span<const std::byte>> {
            if (!HasRange(bytes, offset, size) || offset > std::numeric_limits<size_t>::max() ||
                size > std::numeric_limits<size_t>::max()) {
                return std::unexpected(ErrorCode::DataTruncated);
            }
            return bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
        }

    } // namespace Detail

    /** @brief Read @p T from @p bytes at @p offset as a little-endian scalar without bounds checks. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr T ReadLittleEndianUnchecked(std::span<const std::byte> bytes, size_t offset) noexcept {
        using U = Detail::UnsignedScalar<T>;
        U value = 0;
        for (size_t i = 0; i < sizeof(U); ++i) {
            value |= static_cast<U>(std::to_integer<uint8_t>(bytes[offset + i])) << (i * 8u);
        }
        return Detail::FromUnsigned<T>(value);
    }

    /** @brief Read @p T from @p bytes at @p offset as a big-endian scalar without bounds checks. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr T ReadBigEndianUnchecked(std::span<const std::byte> bytes, size_t offset) noexcept {
        using U = Detail::UnsignedScalar<T>;
        U value = 0;
        for (size_t i = 0; i < sizeof(U); ++i) {
            value = static_cast<U>((value << 8u) | std::to_integer<uint8_t>(bytes[offset + i]));
        }
        return Detail::FromUnsigned<T>(value);
    }

    /** @brief Read @p T from @p bytes at @p offset as an endian-selected scalar without bounds checks. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr T ReadEndianUnchecked(std::span<const std::byte> bytes, size_t offset,
                                                  bool littleEndian) noexcept {
        return littleEndian ? ReadLittleEndianUnchecked<T>(bytes, offset) : ReadBigEndianUnchecked<T>(bytes, offset);
    }

    /** @brief Read a little-endian scalar and advance @p offset. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr auto ReadLittleEndian(std::span<const std::byte> bytes, size_t& offset) -> Result<T> {
        using U = Detail::UnsignedScalar<T>;
        if (offset > bytes.size() || sizeof(U) > bytes.size() - offset) {
            return std::unexpected(ErrorCode::DataTruncated);
        }
        auto value = ReadLittleEndianUnchecked<T>(bytes, offset);
        offset += sizeof(U);
        return value;
    }

    /** @brief Read a big-endian scalar and advance @p offset. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr auto ReadBigEndian(std::span<const std::byte> bytes, size_t& offset) -> Result<T> {
        using U = Detail::UnsignedScalar<T>;
        if (offset > bytes.size() || sizeof(U) > bytes.size() - offset) {
            return std::unexpected(ErrorCode::DataTruncated);
        }
        auto value = ReadBigEndianUnchecked<T>(bytes, offset);
        offset += sizeof(U);
        return value;
    }

    /** @brief Read an endian-selected scalar and advance @p offset. */
    template<Concept::Scalar T>
    [[nodiscard]] constexpr auto ReadEndian(std::span<const std::byte> bytes, size_t& offset, bool littleEndian)
        -> Result<T> {
        return littleEndian ? ReadLittleEndian<T>(bytes, offset) : ReadBigEndian<T>(bytes, offset);
    }

    /** @brief Append @p value as a little-endian scalar. */
    template<Concept::Scalar T>
    constexpr void AppendLittleEndian(std::vector<std::byte>& out, T value) {
        Detail::AppendLittleEndianScalar(out, value);
    }

    /** @brief Append @p value as a big-endian scalar. */
    template<Concept::Scalar T>
    constexpr void AppendBigEndian(std::vector<std::byte>& out, T value) {
        Detail::AppendBigEndianScalar(out, value);
    }

    /** @brief Write @p value as a little-endian scalar without bounds checks. */
    template<Concept::Scalar T, Concept::MutableByte Byte>
    constexpr void WriteLittleEndianUnchecked(std::span<Byte> out, size_t& offset, T value) noexcept {
        auto u = static_cast<Detail::UnsignedScalar<T>>(Sora::CastToUnsigned(value));
        for (size_t i = 0; i < sizeof(u); ++i) {
            Detail::StoreByte(out, offset, static_cast<uint8_t>((u >> (i * 8u)) & 0xFFu));
        }
    }

    /** @brief Write @p value as a big-endian scalar without bounds checks. */
    template<Concept::Scalar T, Concept::MutableByte Byte>
    constexpr void WriteBigEndianUnchecked(std::span<Byte> out, size_t& offset, T value) noexcept {
        auto u = static_cast<Detail::UnsignedScalar<T>>(Sora::CastToUnsigned(value));
        for (size_t i = 0; i < sizeof(u); ++i) {
            const size_t shift = (sizeof(u) - 1u - i) * 8u;
            Detail::StoreByte(out, offset, static_cast<uint8_t>((u >> shift) & 0xFFu));
        }
    }

    /** @brief Write @p value as a little-endian scalar and advance @p offset. */
    template<Concept::Scalar T, Concept::MutableByte Byte>
    constexpr auto WriteLittleEndian(std::span<Byte> out, size_t& offset, T value) -> VoidResult {
        using U = Detail::UnsignedScalar<T>;
        if (offset > out.size() || sizeof(U) > out.size() - offset) {
            return std::unexpected(ErrorCode::DataTruncated);
        }
        WriteLittleEndianUnchecked(out, offset, value);
        return {};
    }

    /** @brief Write @p value as a big-endian scalar and advance @p offset. */
    template<Concept::Scalar T, Concept::MutableByte Byte>
    constexpr auto WriteBigEndian(std::span<Byte> out, size_t& offset, T value) -> VoidResult {
        using U = Detail::UnsignedScalar<T>;
        if (offset > out.size() || sizeof(U) > out.size() - offset) {
            return std::unexpected(ErrorCode::DataTruncated);
        }
        WriteBigEndianUnchecked(out, offset, value);
        return {};
    }

    template<typename T>
    [[nodiscard]] constexpr auto Read(std::span<const std::byte> bytes, size_t& offset) -> Result<T>;

    /** @brief Immutable cursor over a byte image. */
    class Cursor {
    public:
        /** @brief Construct an empty cursor. */
        constexpr Cursor() noexcept = default;

        /** @brief Construct a cursor over @p bytes starting at @p offset. */
        explicit constexpr Cursor(std::span<const std::byte> bytes, size_t offset = 0) noexcept
            : bytes_(bytes), offset_(offset) {}

        /** @brief Return the backing byte image. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept { return bytes_; }

        /** @brief Return the current byte offset. */
        [[nodiscard]] constexpr size_t Offset() const noexcept { return offset_; }

        /** @brief Return remaining readable bytes. */
        [[nodiscard]] constexpr size_t Remaining() const noexcept {
            return offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
        }

        /** @brief Return whether @p size bytes can be read from the current cursor position. */
        [[nodiscard]] constexpr bool CanRead(size_t size) const noexcept { return size <= Remaining(); }

        /** @brief Read a little-endian scalar and advance this cursor. */
        template<Concept::Scalar T>
        [[nodiscard]] constexpr auto ReadLittleEndian() -> Result<T> {
            return Sora::Wire::ReadLittleEndian<T>(bytes_, offset_);
        }

        /** @brief Read a big-endian scalar and advance this cursor. */
        template<Concept::Scalar T>
        [[nodiscard]] constexpr auto ReadBigEndian() -> Result<T> {
            return Sora::Wire::ReadBigEndian<T>(bytes_, offset_);
        }

        /** @brief Read an endian-selected scalar and advance this cursor. */
        template<Concept::Scalar T>
        [[nodiscard]] constexpr auto ReadEndian(bool littleEndian) -> Result<T> {
            return Sora::Wire::ReadEndian<T>(bytes_, offset_, littleEndian);
        }

        /** @brief Read a reflected little-endian schema and advance this cursor. */
        template<typename T>
        [[nodiscard]] constexpr auto Read() -> Result<T> {
            return Sora::Wire::Read<T>(bytes_, offset_);
        }

    private:
        std::span<const std::byte> bytes_{};
        size_t offset_ = 0;
    };

    /** @brief Serialized size of a reflected little-endian schema. */
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
        throw "Sora::Wire::OffsetOf: member does not belong to schema.";
    }

    /** @brief Append a reflected schema value to @p out using little-endian scalar fields. */
    template<typename T>
    constexpr void Append(std::vector<std::byte>& out, const T& value) {
        Detail::ValidateSchema<T>();
        out.reserve(out.size() + SizeOf<T>());
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            AppendLittleEndian(out, value.[:member:]);
        }
    }

    /** @brief Write a reflected schema value into a caller-proven byte buffer at @p offset. */
    template<typename T, Concept::MutableByte Byte>
    constexpr void WriteUnchecked(std::span<Byte> out, size_t offset, const T& value) noexcept {
        Detail::ValidateSchema<T>();
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            WriteLittleEndianUnchecked(out, offset, value.[:member:]);
        }
    }

    /** @brief Write a reflected schema value into a caller-proven static byte array at @p offset. */
    template<typename T, Concept::MutableByte Byte, size_t N>
    constexpr void WriteUnchecked(std::array<Byte, N>& out, size_t offset, const T& value) noexcept {
        WriteUnchecked(std::span<Byte>{out}, offset, value);
    }

    /** @brief Overwrite a reflected schema value at byte offset @p offset in @p out. */
    template<typename T>
    constexpr auto WriteAt(std::vector<std::byte>& out, size_t offset, const T& value) -> VoidResult {
        constexpr size_t size = SizeOf<T>();
        if (offset > out.size() || size > out.size() - offset) {
            return std::unexpected(ErrorCode::InvalidArgument);
        }
        WriteUnchecked(std::span<std::byte>{out}, offset, value);
        return {};
    }

    /** @brief Read a reflected little-endian schema value from @p bytes at @p offset. */
    template<typename T>
    [[nodiscard]] constexpr auto Read(std::span<const std::byte> bytes, size_t& offset) -> Result<T> {
        Detail::ValidateSchema<T>();
        T value{};
        template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
            using Field = typename [:std::meta::type_of(member):];
            auto field = ReadLittleEndian<Field>(bytes, offset);
            if (!field) {
                return std::unexpected(field.error());
            }
            value.[:member:] = *field;
        }
        return value;
    }

} // namespace Sora::Wire
