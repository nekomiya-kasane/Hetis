#include <Sora/Core/PAL/Clipboard.h>
#include <Sora/Core/PAL/Module.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace Clipboard = Sora::PAL::Clipboard;

TEST_CASE("Clipboard backend resolves independent native capability tables", "[Sora.PAL.Clipboard]") {
#if defined(PLATFORM_WINDOWS)
    const Sora::PAL::NativeErrorSystemAPI& error = Sora::PAL::LoadNativeErrorSystemAPI();
    REQUIRE(error.getLastError != nullptr);
    REQUIRE(error.setLastError != nullptr);

    const Sora::PAL::GlobalMemorySystemAPI& memory = Sora::PAL::LoadGlobalMemorySystemAPI();
    REQUIRE(memory.globalAllocate != nullptr);
    REQUIRE(memory.globalFree != nullptr);
    REQUIRE(memory.globalLock != nullptr);
    REQUIRE(memory.globalUnlock != nullptr);
    REQUIRE(memory.globalSize != nullptr);

    const Sora::PAL::ClipboardSystemAPI& clipboard = Sora::PAL::LoadClipboardSystemAPI();
    REQUIRE(clipboard.openClipboard != nullptr);
    REQUIRE(clipboard.closeClipboard != nullptr);
    REQUIRE(clipboard.emptyClipboard != nullptr);
    REQUIRE(clipboard.getClipboardData != nullptr);
    REQUIRE(clipboard.setClipboardData != nullptr);
    REQUIRE(clipboard.isClipboardFormatAvailable != nullptr);
#else
    SUCCEED("The native clipboard backend is unavailable on this platform.");
#endif
}

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
    using CountClipboardFormatsFunction = int(__stdcall*)();
    constexpr Sora::PAL::ModuleLoadOptions options{
        .nameKind = Sora::PAL::ModuleNameKind::ExactPath,
        .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
        .cachePolicy = Sora::PAL::ModuleCachePolicy::Private,
    };
    const Sora::Result<Sora::PAL::ModulePtr> user = Sora::PAL::LoadModule({"user32.dll"}, options);
    REQUIRE(user.has_value());
    const auto countClipboardFormats =
        (*user)->TryFindFunction<CountClipboardFormatsFunction>("CountClipboardFormats");
    REQUIRE(countClipboardFormats != nullptr);

    const Sora::PAL::NativeErrorSystemAPI& error = Sora::PAL::LoadNativeErrorSystemAPI();
    REQUIRE(error.setLastError != nullptr);
    REQUIRE(error.getLastError != nullptr);
    error.setLastError(Sora::PAL::WindowsSystem::kErrorSuccess);
    const int initialFormatCount = countClipboardFormats();
    if (initialFormatCount != 0) {
        SKIP("The live test requires an initially empty clipboard so it cannot destroy user-owned formats.");
    }
    if (error.getLastError() != Sora::PAL::WindowsSystem::kErrorSuccess) {
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
