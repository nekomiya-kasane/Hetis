#include <Sora/Core/PAL/Clipboard.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

namespace Clipboard = Sora::PAL::Clipboard;

TEST_CASE("Clipboard rejects malformed UTF-8 before touching the native service", "[Sora.PAL.Clipboard]") {
    const auto written = Clipboard::WriteText("\xF0\x28\x8C\x28");

    REQUIRE_FALSE(written.has_value());
    if constexpr (Sora::Platform::kIsWindows) {
        REQUIRE(written.error() == Sora::ErrorCode::InvalidUtf8Continuation);
    } else {
        REQUIRE(written.error() == Sora::ErrorCode::NotSupported);
    }
}

TEST_CASE("Clipboard exposes a coherent native Unicode-text snapshot", "[Sora.PAL.Clipboard]") {
    const auto text = Clipboard::ReadText();

    if constexpr (Sora::Platform::kIsWindows) {
        REQUIRE(text.has_value());
        if (*text) {
            REQUIRE(Sora::Unicode::Utf8ToUtf16(**text).has_value());
        }
        REQUIRE(Clipboard::HasText().has_value());
    } else {
        REQUIRE_FALSE(text.has_value());
        REQUIRE(text.error() == Sora::ErrorCode::NotSupported);
        REQUIRE_FALSE(Clipboard::HasText().has_value());
        REQUIRE_FALSE(Clipboard::Clear().has_value());
    }
}

TEST_CASE("Clipboard round-trips Unicode text and distinguishes empty text from no text",
          "[.][live-clipboard][Sora.PAL.Clipboard]") {
#ifndef _WIN32
    SKIP("No native clipboard backend exists for this target.");
#else
    ::SetLastError(ERROR_SUCCESS);
    const int initialFormatCount = ::CountClipboardFormats();
    if (initialFormatCount != 0) {
        SKIP("The live test requires an initially empty clipboard so it cannot destroy user-owned formats.");
    }
    if (::GetLastError() != ERROR_SUCCESS) {
        SKIP("The current desktop session does not expose a testable clipboard.");
    }

    struct ClearOnExit {
        ~ClearOnExit() { static_cast<void>(Clipboard::Clear()); }
    } clearOnExit;

    constexpr std::string_view sample = "Sora clipboard: ASCII, \xE4\xB8\xAD\xE6\x96\x87, \xF0\x9F\x98\x80";
    REQUIRE(Clipboard::WriteText(sample).has_value());
    const auto read = Clipboard::ReadText();
    REQUIRE(read.has_value());
    REQUIRE(read->has_value());
    REQUIRE(**read == sample);
    const auto populatedHasText = Clipboard::HasText();
    REQUIRE(populatedHasText.has_value());
    REQUIRE(*populatedHasText);

    REQUIRE(Clipboard::WriteText("").has_value());
    const auto empty = Clipboard::ReadText();
    REQUIRE(empty.has_value());
    REQUIRE(empty->has_value());
    REQUIRE(empty->value().empty());
    const auto emptyHasText = Clipboard::HasText();
    REQUIRE(emptyHasText.has_value());
    REQUIRE(*emptyHasText);

    REQUIRE(Clipboard::Clear().has_value());
    const auto cleared = Clipboard::ReadText();
    REQUIRE(cleared.has_value());
    REQUIRE_FALSE(cleared->has_value());
    const auto clearedHasText = Clipboard::HasText();
    REQUIRE(clearedHasText.has_value());
    REQUIRE_FALSE(*clearedHasText);
#endif
}
