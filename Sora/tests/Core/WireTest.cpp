#include <Sora/Core/Wire.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace Sora;

namespace {

    enum class WireKind : uint16_t { Alpha = 0x1122, Beta = 0x3344 };

    struct WireRecord {
        uint16_t kind = 0;
        uint32_t count = 0;
        WireKind tag = WireKind::Alpha;
        uint64_t hash = 0;
    };

    static_assert(Wire::SizeOf<WireRecord>() == 16);
    static_assert(Wire::OffsetOf<WireRecord, ^^WireRecord::kind>() == 0);
    static_assert(Wire::OffsetOf<WireRecord, ^^WireRecord::count>() == 2);
    static_assert(Wire::OffsetOf<WireRecord, ^^WireRecord::tag>() == 6);
    static_assert(Wire::OffsetOf<WireRecord, ^^WireRecord::hash>() == 8);

} // namespace

TEST_CASE("Wire scalar helpers read and write endian-stable integers", "[Sora][Core][Wire]") {
    std::vector<std::byte> bytes;
    Wire::AppendLittleEndian(bytes, uint16_t{0x1234});
    Wire::AppendBigEndian(bytes, uint32_t{0xA1B2C3D4});

    REQUIRE(bytes == std::vector<std::byte>{std::byte{0x34}, std::byte{0x12}, std::byte{0xA1}, std::byte{0xB2},
                                            std::byte{0xC3}, std::byte{0xD4}});

    size_t offset = 0;
    auto little = Wire::ReadLittleEndian<uint16_t>(bytes, offset);
    REQUIRE(little.has_value());
    REQUIRE(*little == 0x1234);
    auto big = Wire::ReadBigEndian<uint32_t>(bytes, offset);
    REQUIRE(big.has_value());
    REQUIRE(*big == 0xA1B2C3D4);
    REQUIRE(offset == bytes.size());

    auto truncated = Wire::ReadLittleEndian<uint16_t>(bytes, offset);
    REQUIRE_FALSE(truncated.has_value());
    REQUIRE(truncated.error() == ErrorCode::DataTruncated);
}

TEST_CASE("Wire reflected schemas serialize without object-padding dependence", "[Sora][Core][Wire]") {
    const WireRecord record{.kind = 0x0102, .count = 0xA0B0C0D0, .tag = WireKind::Beta, .hash = 0x0102030405060708};
    std::vector<std::byte> bytes;
    Wire::Append(bytes, record);

    REQUIRE(bytes.size() == Wire::SizeOf<WireRecord>());
    REQUIRE(bytes[0] == std::byte{0x02});
    REQUIRE(bytes[1] == std::byte{0x01});
    REQUIRE(bytes[2] == std::byte{0xD0});
    REQUIRE(bytes[6] == std::byte{0x44});
    REQUIRE(bytes[7] == std::byte{0x33});
    REQUIRE(bytes[8] == std::byte{0x08});
    REQUIRE(bytes[15] == std::byte{0x01});

    size_t offset = 0;
    auto decoded = Wire::Read<WireRecord>(bytes, offset);
    REQUIRE(decoded.has_value());
    REQUIRE(offset == Wire::SizeOf<WireRecord>());
    REQUIRE(decoded->kind == record.kind);
    REQUIRE(decoded->count == record.count);
    REQUIRE(decoded->tag == record.tag);
    REQUIRE(decoded->hash == record.hash);
}

TEST_CASE("Wire cursor and fixed-buffer writes share checked byte-range semantics", "[Sora][Core][Wire]") {
    std::array<unsigned char, Wire::SizeOf<WireRecord>()> storage{};
    const WireRecord record{.kind = 9, .count = 10, .tag = WireKind::Alpha, .hash = 11};
    Wire::WriteUnchecked(storage, 0, record);

    auto bytes = std::as_bytes(std::span{storage});
    REQUIRE(Wire::HasRange(bytes, 0, bytes.size()));
    REQUIRE_FALSE(Wire::HasRange(bytes, bytes.size(), 1));

    Wire::Cursor cursor{bytes};
    auto decoded = cursor.Read<WireRecord>();
    REQUIRE(decoded.has_value());
    REQUIRE(cursor.Offset() == Wire::SizeOf<WireRecord>());
    REQUIRE(decoded->kind == record.kind);
    REQUIRE(decoded->count == record.count);
    REQUIRE(decoded->hash == record.hash);

    std::vector<std::byte> dynamic(bytes.begin(), bytes.end());
    WireRecord replacement{.kind = 0xEEFF, .count = 1, .tag = WireKind::Beta, .hash = 2};
    REQUIRE(Wire::WriteAt(dynamic, 0, replacement).has_value());
    size_t offset = 0;
    auto rewritten = Wire::Read<WireRecord>(dynamic, offset);
    REQUIRE(rewritten.has_value());
    REQUIRE(rewritten->kind == replacement.kind);
    REQUIRE(rewritten->tag == replacement.tag);
}
