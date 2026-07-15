#include <Sora/Core/Unicode.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

using Sora::ErrorCode;
using Sora::Unicode::InvalidSequencePolicy;

namespace {

    inline constexpr auto kReplace = InvalidSequencePolicy::Replace;

} // namespace

static_assert(Sora::Unicode::Concept::Utf16CodeUnit<char16_t>);
static_assert(Sora::Unicode::Concept::Utf16CodeUnit<uint16_t>);
static_assert(!Sora::Unicode::Concept::Utf16CodeUnit<int16_t>);
static_assert(!Sora::Unicode::Concept::Utf16CodeUnit<char>);
static_assert(Sora::Unicode::Concept::Utf16CodeUnit<wchar_t> == (sizeof(wchar_t) == sizeof(uint16_t)));

TEST_CASE("Unicode strictly transcodes UTF-8 and UTF-16 in constexpr evaluation", "[Sora.Core.Unicode]") {
    constexpr std::string_view utf8 = "ASCII \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";
    const auto utf16 = Sora::Unicode::Utf8ToUtf16(utf8);
    constexpr auto supplementary = Sora::Unicode::Utf8ToUtf16("A\xF0\x9F\x98\x80");
    constexpr auto empty = Sora::Unicode::Utf8ToUtf16("");

    REQUIRE(utf16.has_value());
    STATIC_REQUIRE(supplementary.has_value());
    STATIC_REQUIRE(*supplementary == u"A\U0001F600");
    STATIC_REQUIRE(empty->empty());
    REQUIRE(*utf16 == u"ASCII \u4E2D\u6587 \U0001F600");

    const auto roundTrip = Sora::Unicode::Utf16ToUtf8(std::u16string_view{*utf16});
    REQUIRE(roundTrip.has_value());
    REQUIRE(*roundTrip == utf8);
}

TEST_CASE("Unicode bridges char8_t UTF-8 code units to canonical char storage", "[Sora.Core.Unicode]") {
    constexpr auto converted = Sora::Unicode::Utf8BytesToString(u8"ASCII 中文 😀");
    STATIC_REQUIRE(converted == "ASCII \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80");

    const std::u8string runtime = u8"runtime 中文";
    REQUIRE(Sora::Unicode::Utf8BytesToString(runtime) == "runtime \xE4\xB8\xAD\xE6\x96\x87");
}

TEST_CASE("Unicode reports precise malformed UTF-8 categories", "[Sora.Core.Unicode]") {
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\x80").error() == ErrorCode::InvalidUtf8LeadingByte);
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\xC0\xAF").error() == ErrorCode::OverlongUtf8Sequence);
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\xE2\x28\xA1").error() == ErrorCode::InvalidUtf8Continuation);
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\xE2\x82").error() == ErrorCode::TruncatedUtf8Sequence);
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\xED\xA0\x80").error() == ErrorCode::InvalidScalar);
    STATIC_REQUIRE(Sora::Unicode::Utf8ToUtf16("\xF4\x90\x80\x80").error() == ErrorCode::InvalidScalar);
}

TEST_CASE("Unicode replacement policy is explicit and symmetric", "[Sora.Core.Unicode]") {
    constexpr std::array<char16_t, 2> malformedUtf16 = {char16_t{0xD800}, u'A'};
    constexpr auto rejectedUtf16 = Sora::Unicode::Utf16ToUtf8(std::span{malformedUtf16});
    constexpr auto replacedUtf16 = Sora::Unicode::Utf16ToUtf8<kReplace>(std::span{malformedUtf16});
    constexpr auto rejectedUtf8 = Sora::Unicode::Utf8ToUtf32("\xE2\x28\xA1");
    constexpr auto replacedUtf8 = Sora::Unicode::Utf8ToUtf32<kReplace>("\xE2\x28\xA1");
    constexpr auto replacedOverlong = Sora::Unicode::Utf8ToUtf32<kReplace>("\xE0\x80\x80");
    static constexpr std::array<char32_t, 1> malformedUtf32Storage = {char32_t{0xD800}};
    constexpr std::u32string_view malformedUtf32{malformedUtf32Storage.data(), malformedUtf32Storage.size()};
    constexpr auto rejectedUtf32 = Sora::Unicode::Utf32ToUtf8(malformedUtf32);
    constexpr auto replacedUtf32 = Sora::Unicode::Utf32ToUtf8<kReplace>(malformedUtf32);
    constexpr auto rejectedUtf8Scalar = Sora::Unicode::EncodeUtf8Scalar(char32_t{0xD800});
    constexpr auto replacedUtf8Scalar = Sora::Unicode::EncodeUtf8Scalar<kReplace>(char32_t{0xD800});
    constexpr auto rejectedUtf16Scalar = Sora::Unicode::EncodeUtf16Scalar(char32_t{0x110000});
    constexpr auto replacedUtf16Scalar = Sora::Unicode::EncodeUtf16Scalar<kReplace>(char32_t{0x110000});

    STATIC_REQUIRE(rejectedUtf16.error() == ErrorCode::IsolatedHighSurrogate);
    STATIC_REQUIRE(replacedUtf16 == "\xEF\xBF\xBD\x41");
    STATIC_REQUIRE(rejectedUtf8.error() == ErrorCode::InvalidUtf8Continuation);
    STATIC_REQUIRE(replacedUtf8.has_value());
    STATIC_REQUIRE(*replacedUtf8 == U"\uFFFD(\uFFFD");
    STATIC_REQUIRE(replacedOverlong.has_value());
    STATIC_REQUIRE(*replacedOverlong == U"\uFFFD\uFFFD\uFFFD");
    STATIC_REQUIRE(rejectedUtf32.error() == ErrorCode::InvalidScalar);
    STATIC_REQUIRE(replacedUtf32.has_value());
    STATIC_REQUIRE(*replacedUtf32 == "\xEF\xBF\xBD");
    STATIC_REQUIRE(rejectedUtf8Scalar.error() == ErrorCode::InvalidScalar);
    STATIC_REQUIRE(replacedUtf8Scalar->View() == "\xEF\xBF\xBD");
    STATIC_REQUIRE(rejectedUtf16Scalar.error() == ErrorCode::InvalidScalar);
    STATIC_REQUIRE(replacedUtf16Scalar->size == 1);
    STATIC_REQUIRE(replacedUtf16Scalar->units[0] == 0xFFFD);
}

TEST_CASE("Unicode replacement consumes each maximal malformed UTF-8 subpart", "[Sora.Core.Unicode]") {
    constexpr std::array cases = {
        std::pair<std::string_view, std::u32string_view>{"\xC0\xAF", U"\uFFFD\uFFFD"},
        std::pair<std::string_view, std::u32string_view>{"\xE0\x80\x80", U"\uFFFD\uFFFD\uFFFD"},
        std::pair<std::string_view, std::u32string_view>{"\xED\xA0\x80", U"\uFFFD\uFFFD\uFFFD"},
        std::pair<std::string_view, std::u32string_view>{"\xF4\x90\x80\x80", U"\uFFFD\uFFFD\uFFFD\uFFFD"},
        std::pair<std::string_view, std::u32string_view>{"\xE1\x80\x41", U"\uFFFDA"},
        std::pair<std::string_view, std::u32string_view>{"\xF0\x90\x80", U"\uFFFD"},
        std::pair<std::string_view, std::u32string_view>{"\xE2\x28\xA1", U"\uFFFD(\uFFFD"},
    };

    for (const auto& [input, expected] : cases) {
        const auto actual = Sora::Unicode::Utf8ToUtf32<kReplace>(input);
        REQUIRE(actual.has_value());
        REQUIRE(*actual == expected);
    }
}

TEST_CASE("Unicode append APIs preserve prefixes and roll back rejected input", "[Sora.Core.Unicode]") {
    const std::array<char16_t, 2> malformed = {char16_t{0xD800}, u'A'};
    std::string utf8 = "prefix:";
    const auto rejected = Sora::Unicode::AppendUtf16ToUtf8(utf8, std::span{malformed});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == ErrorCode::IsolatedHighSurrogate);
    REQUIRE(utf8 == "prefix:");

    REQUIRE(Sora::Unicode::AppendUtf16ToUtf8<kReplace>(utf8, std::span{malformed}).has_value());
    REQUIRE(utf8 == "prefix:\xEF\xBF\xBD\x41");

    std::u16string utf16 = u"prefix:";
    std::u32string utf32 = U"prefix:";
    std::wstring wide = L"prefix:";
    REQUIRE(Sora::Unicode::AppendUtf8ToUtf16(utf16, "\xF0\x9F\x98\x80").has_value());
    REQUIRE(Sora::Unicode::AppendUtf8ToUtf32(utf32, "\xF0\x9F\x98\x80").has_value());
    REQUIRE(Sora::Unicode::AppendUtf8ToWide(wide, "\xF0\x9F\x98\x80").has_value());
    REQUIRE(utf16 == u"prefix:\U0001F600");
    REQUIRE(utf32 == U"prefix:\U0001F600");

    const auto wideUtf8 = Sora::Unicode::WideToUtf8(wide);
    REQUIRE(wideUtf8.has_value());
    REQUIRE(*wideUtf8 == "prefix:\xF0\x9F\x98\x80");

    std::u16string rejectedUtf16 = u"prefix:";
    const auto invalidUtf8 = Sora::Unicode::AppendUtf8ToUtf16(rejectedUtf16, "\xE2\x82");
    REQUIRE_FALSE(invalidUtf8.has_value());
    REQUIRE(invalidUtf8.error() == ErrorCode::TruncatedUtf8Sequence);
    REQUIRE(rejectedUtf16 == u"prefix:");

    std::u32string rejectedUtf32 = U"prefix:";
    std::wstring rejectedWide = L"prefix:";
    REQUIRE_FALSE(Sora::Unicode::AppendUtf8ToUtf32(rejectedUtf32, "\xED\xA0\x80").has_value());
    REQUIRE_FALSE(Sora::Unicode::AppendUtf8ToWide(rejectedWide, "\xF4\x90\x80\x80").has_value());
    REQUIRE(rejectedUtf32 == U"prefix:");
    REQUIRE(rejectedWide == L"prefix:");

    std::string rejectedScalar = "prefix:";
    std::string rejectedUtf32Input = "prefix:";
    const std::array invalidUtf32 = {char32_t{0x110000}};
    REQUIRE_FALSE(Sora::Unicode::AppendUtf8Scalar(rejectedScalar, char32_t{0xD800}).has_value());
    REQUIRE_FALSE(Sora::Unicode::AppendUtf32ToUtf8(rejectedUtf32Input,
                                                   std::u32string_view{invalidUtf32.data(), invalidUtf32.size()})
                      .has_value());
    REQUIRE(rejectedScalar == "prefix:");
    REQUIRE(rejectedUtf32Input == "prefix:");

    std::wstring invalidWide;
    if constexpr (sizeof(wchar_t) == sizeof(uint16_t)) {
        invalidWide.push_back(static_cast<wchar_t>(0xD800));
    } else {
        invalidWide.push_back(static_cast<wchar_t>(0x110000));
    }
    std::string rejectedWideInput = "prefix:";
    REQUIRE_FALSE(Sora::Unicode::AppendWideToUtf8(rejectedWideInput, invalidWide).has_value());
    REQUIRE(rejectedWideInput == "prefix:");
}

TEST_CASE("Unicode append APIs reuse caller-provided output capacity", "[Sora.Core.Unicode]") {
    std::string utf8;
    std::u16string utf16;
    std::u32string utf32;
    std::wstring wide;
    utf8.reserve(64);
    utf16.reserve(64);
    utf32.reserve(64);
    wide.reserve(64);
    const auto utf8Capacity = utf8.capacity();
    const auto utf16Capacity = utf16.capacity();
    const auto utf32Capacity = utf32.capacity();
    const auto wideCapacity = wide.capacity();

    REQUIRE(Sora::Unicode::AppendUtf16ToUtf8(utf8, u"A\U0001F600").has_value());
    REQUIRE(Sora::Unicode::AppendUtf8ToUtf16(utf16, "A\xF0\x9F\x98\x80").has_value());
    REQUIRE(Sora::Unicode::AppendUtf8ToUtf32(utf32, "A\xF0\x9F\x98\x80").has_value());
    REQUIRE(Sora::Unicode::AppendUtf8ToWide(wide, "A\xF0\x9F\x98\x80").has_value());
    REQUIRE(utf8.capacity() == utf8Capacity);
    REQUIRE(utf16.capacity() == utf16Capacity);
    REQUIRE(utf32.capacity() == utf32Capacity);
    REQUIRE(wide.capacity() == wideCapacity);
}

TEST_CASE("Unicode stream decoder exposes no contradictory error state", "[Sora.Core.Unicode]") {
    Sora::Unicode::Utf16StreamDecoder<> strict;
    const auto pending = strict.Push(0xD83D);
    REQUIRE(pending.has_value());
    REQUIRE(pending->Empty());
    REQUIRE(strict.HasPendingHighSurrogate());

    const auto rejected = strict.Push(u'A');
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == ErrorCode::IsolatedHighSurrogate);
    REQUIRE_FALSE(strict.HasPendingHighSurrogate());

    Sora::Unicode::Utf16StreamDecoder<kReplace> replacing;
    REQUIRE(replacing.Push(0xD83D).has_value());
    const auto recovered = replacing.Push(u'A');
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->Size() == 2);
    REQUIRE((*recovered)[0] == Sora::Unicode::kReplacementCharacter);
    REQUIRE((*recovered)[1] == U'A');
    REQUIRE(replacing.Flush()->Empty());

    Sora::Unicode::Utf16StreamDecoder<> strictTail;
    REQUIRE(strictTail.Push(0xD83D).has_value());
    const auto rejectedTail = strictTail.Flush();
    REQUIRE_FALSE(rejectedTail.has_value());
    REQUIRE(rejectedTail.error() == ErrorCode::IsolatedHighSurrogate);
    REQUIRE_FALSE(strictTail.HasPendingHighSurrogate());

    Sora::Unicode::Utf16StreamDecoder<kReplace> replacedTail;
    REQUIRE(replacedTail.Push(0xD83D).has_value());
    const auto recoveredTail = replacedTail.Flush();
    REQUIRE(recoveredTail.has_value());
    REQUIRE(recoveredTail->Size() == 1);
    REQUIRE((*recoveredTail)[0] == Sora::Unicode::kReplacementCharacter);
}

TEST_CASE("Unicode scalar codecs preserve every scalar value", "[Sora.Core.Unicode]") {
    bool valid = true;
    char32_t failed = 0;
    for (char32_t codepoint = 0; codepoint <= Sora::Unicode::kMaxScalarValue; ++codepoint) {
        if (!Sora::Unicode::IsScalarValue(codepoint)) {
            continue;
        }

        const auto utf8 = Sora::Unicode::EncodeUtf8Scalar(codepoint);
        const auto utf16 = Sora::Unicode::EncodeUtf16Scalar(codepoint);
        if (!utf8 || !utf16) {
            valid = false;
            failed = codepoint;
            break;
        }
        const auto decoded = Sora::Unicode::DecodeUtf8Scalar(utf8->View());
        Sora::Unicode::Utf16StreamDecoder<> decoder;
        auto first = decoder.Push(utf16->units[0]);
        auto second = utf16->size == 2
                          ? decoder.Push(utf16->units[1])
                          : Sora::Result<Sora::Unicode::Utf16DecodeResult>{Sora::Unicode::Utf16DecodeResult{}};
        const auto tail = decoder.Flush();
        const auto& scalarResult = utf16->size == 2 ? second : first;
        if (!decoded || decoded->value != codepoint || !first || !second || !tail || scalarResult->Size() != 1 ||
            (*scalarResult)[0] != codepoint || !tail->Empty()) {
            valid = false;
            failed = codepoint;
            break;
        }
    }
    CAPTURE(static_cast<uint32_t>(failed));
    REQUIRE(valid);
}
