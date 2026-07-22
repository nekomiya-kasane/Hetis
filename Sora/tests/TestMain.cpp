/**
 * @file TestMain.cpp
 * @brief Sora Catch2 process entry point with crash handling and styled bootstrap diagnostics.
 * @details The default variant installs Sora::CrashHandler before Catch2 starts. Catch2 may temporarily layer its
 * per-test fatal-condition reporter over this handler and cooperatively chains to the preceding native handler.
 * @ingroup Testing
 */

#ifndef SORA_TEST_INSTALL_CRASH_HANDLER
#    error "SORA_TEST_INSTALL_CRASH_HANDLER must be defined by the Sora test-runner target."
#endif

#if SORA_TEST_INSTALL_CRASH_HANDLER
#    include <Sora/Core/CrashHandler.h>
#    include <Sora/Core/ToStyledString.h>
#endif

#include <catch2/catch_session.hpp>

#if SORA_TEST_INSTALL_CRASH_HANDLER
#    include <cstdio>
#    include <cstdlib>
#    include <string>
#    include <string_view>
#    include <utility>
#endif

#if SORA_TEST_INSTALL_CRASH_HANDLER
namespace {

    /** @brief Write one complete runner diagnostic to the native standard-error stream. */
    void WriteStandardError(std::string_view message) noexcept {
        static_cast<void>(std::fwrite(message.data(), sizeof(char), message.size(), stderr));
        static_cast<void>(std::fflush(stderr));
    }

    /** @brief Report a recoverable CrashHandler installation failure through Sora's styled-output protocol. */
    [[nodiscard]] int ReportCrashHandlerFailure(Sora::ErrorCode error) noexcept {
        try {
            const bool color = tapioca::terminal::is_tty(stderr) && std::getenv("NO_COLOR") == nullptr;
            const tapioca::terminal_caps caps =
                color ? tapioca::terminal_caps::detect() : tapioca::terminal_caps::legacy_win_cmd();
            Sora::Styled::StyledStringBuilder builder{{
                .color = color,
                .escapePolicy = Sora::Styled::StyledEscapePolicy::TerminalSafe,
                .caps = caps,
            }};
            builder.Text(Sora::Styled::StyledRole::Error, "Sora test runner");
            builder.Raw(Sora::Styled::StyledRole::Punctuation, ": ");
            builder.Raw(Sora::Styled::StyledRole::Plain, "failed to install ");
            builder.Text(Sora::Styled::StyledRole::TypeName, "CrashHandler");
            builder.Raw(Sora::Styled::StyledRole::Plain, " (");
            builder.Text(Sora::Styled::StyledRole::EnumName, Sora::ToString(error));
            builder.Raw(Sora::Styled::StyledRole::Plain, ")");
            std::string message = std::move(builder).Finish();
            message.push_back('\n');
            WriteStandardError(message);
        } catch (...) {
            WriteStandardError("Sora test runner: failed to install CrashHandler\n");
        }
        return EXIT_FAILURE;
    }

} // namespace
#endif

int main(int argc, char* argv[]) {
#if SORA_TEST_INSTALL_CRASH_HANDLER
    auto installed = Sora::CrashHandler::Install({
        .mirrorToStandardError = true,
        .suppressSystemErrorDialogs = true,
    });
    if (!installed) {
        return ReportCrashHandlerFailure(installed.error());
    }
    [[maybe_unused]] Sora::CrashHandler crashHandler = std::move(*installed);
#endif

    return Catch::Session{}.run(argc, argv);
}
