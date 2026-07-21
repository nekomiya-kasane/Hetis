#include <Sora/Core/Assertion.h>
#include <Sora/Core/CrashHandler.h>
#include <Sora/Core/Debugging.h>
#include <Sora/Core/StackTrace.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>

namespace {

    std::atomic<uint32_t> gReportCount{0};
    std::atomic<bool> gReportMatched{false};

    void InspectFailure(const Sora::AssertionFailure& failure) noexcept {
        const bool matched = failure.kind == Sora::AssertionKind::Verification && failure.condition.empty() &&
                             failure.message == "evaluation 2" && failure.stackTrace == nullptr &&
                             failure.source.line() != 0 &&
                             std::string_view{failure.source.file_name()}.ends_with("DiagnosticsTest.cpp");
        gReportMatched.store(matched, std::memory_order_relaxed);
        gReportCount.fetch_add(1, std::memory_order_relaxed);
    }

    class ScopedAssertionConfiguration {
    public:
        ScopedAssertionConfiguration(Sora::AssertionSettings settings, Sora::AssertionReporter reporter) noexcept
            : settings_(Sora::SetAssertionSettings(settings)), reporter_(Sora::SetAssertionReporter(reporter)) {}

        ~ScopedAssertionConfiguration() {
            [[maybe_unused]] const auto currentReporter = Sora::SetAssertionReporter(reporter_);
            [[maybe_unused]] const auto currentSettings = Sora::SetAssertionSettings(settings_);
        }

        ScopedAssertionConfiguration(const ScopedAssertionConfiguration&) = delete;
        ScopedAssertionConfiguration& operator=(const ScopedAssertionConfiguration&) = delete;

    private:
        Sora::AssertionSettings settings_{};
        Sora::AssertionReporter reporter_ = nullptr;
    };

} // namespace

TEST_CASE("Sora verification evaluates once and materializes diagnostics only on failure", "[Sora.Core.Diagnostics]") {
    const ScopedAssertionConfiguration configuration{
        {.action = Sora::AssertionAction::Continue, .captureStackTrace = false}, InspectFailure};
    gReportCount.store(0, std::memory_order_relaxed);
    gReportMatched.store(false, std::memory_order_relaxed);

    int evaluations = 0;
    REQUIRE(Verify(++evaluations == 1, "this message is never formatted {}", evaluations));
    REQUIRE_FALSE(Verify(++evaluations == 3, "evaluation {}", evaluations));

    REQUIRE(evaluations == 2);
    REQUIRE(gReportCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(gReportMatched.load(std::memory_order_relaxed));
}

TEST_CASE("Sora crash-handler registration has exclusive RAII ownership", "[Sora.Core.Diagnostics]") {
    {
        auto handler = Sora::CrashHandler::Install();
        REQUIRE(handler.has_value());
        REQUIRE(static_cast<bool>(*handler));

        auto duplicate = Sora::CrashHandler::Install();
        REQUIRE_FALSE(duplicate.has_value());
        REQUIRE(duplicate.error() == Sora::ErrorCode::InvalidState);
    }

    auto reinstalled = Sora::CrashHandler::Install();
    REQUIRE(reinstalled.has_value());
}

TEST_CASE("Sora crash handler can suppress native error dialogs", "[Sora.Core.Diagnostics]") {
    auto handler = Sora::CrashHandler::Install({.suppressSystemErrorDialogs = true});
    REQUIRE(handler.has_value());
}

TEST_CASE("Sora crash handler eagerly owns and releases its emergency file", "[Sora.Core.Diagnostics]") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / L"sora-crash-handler-test.txt";
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);

    {
        const Sora::CrashHandlerOptions options{
            .emergencyFile = path,
            .callback = nullptr,
            .mirrorToStandardError = false,
        };
        auto handler = Sora::CrashHandler::Install(options);
        REQUIRE(handler.has_value());
        REQUIRE(std::filesystem::exists(path));
    }

    REQUIRE(std::filesystem::remove(path));
}

TEST_CASE("Sora crash handler releases ownership after an invalid emergency path", "[Sora.Core.Diagnostics]") {
    const std::filesystem::path missingDirectory =
        std::filesystem::temp_directory_path() / L"sora-missing-crash-handler-directory";
    std::error_code cleanupError;
    std::filesystem::remove_all(missingDirectory, cleanupError);
    const Sora::CrashHandlerOptions options{
        .emergencyFile = missingDirectory / L"crash.txt",
        .callback = nullptr,
        .mirrorToStandardError = false,
    };

    auto failed = Sora::CrashHandler::Install(options);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error() == Sora::ErrorCode::EmergencyPathInvalid);
    REQUIRE(Sora::CrashHandler::Install().has_value());
}

TEST_CASE("CrashStream rejects an invalid native stream without side effects", "[Sora.Core.Diagnostics]") {
    const Sora::CrashStream stream{};
    REQUIRE_FALSE(static_cast<bool>(stream));
    REQUIRE_FALSE(stream.Write("unreachable"));
    REQUIRE_FALSE(stream.WriteHex(0x1234));
    REQUIRE_FALSE(stream.WriteUnsigned(42));
    stream.Flush();
}

TEST_CASE("StackTrace clamps frame capacity and extreme skip values", "[Sora.Core.Diagnostics]") {
    const Sora::StackTrace bounded = Sora::StackTrace::Capture({.maxFrames = std::numeric_limits<uint32_t>::max()});
    REQUIRE(bounded.Size() <= Sora::StackTrace::kMaximumFrameCount);

    const Sora::StackTrace skipped =
        Sora::StackTrace::Capture({.skipFrames = std::numeric_limits<uint32_t>::max(), .maxFrames = 1});
    REQUIRE(skipped.Empty());
}

TEST_CASE("Debugger detection is a side-effect-free query", "[Sora.Core.Diagnostics]") {
    [[maybe_unused]] const bool attached = Sora::Debug::IsDebuggerPresent();
    SUCCEED();
}
