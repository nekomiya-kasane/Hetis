#include <Sora/Core/Assertion.h>
#include <Sora/Core/CrashHandler.h>
#include <Sora/Core/Debugging.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace {

    std::atomic<uint32_t> gReportCount{0};
    std::atomic<bool> gReportMatched{false};

    void InspectFailure(const Sora::AssertionFailure& failure) noexcept {
        const bool matched = failure.kind == Sora::AssertionKind::Verification &&
                             failure.condition == "++evaluations == 3" && failure.message == "evaluation 2" &&
                             failure.stackTrace == nullptr && failure.source.line() != 0;
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
    REQUIRE(SORA_VERIFY(++evaluations == 1, "this message is never formatted {}", evaluations));
    REQUIRE_FALSE(SORA_VERIFY(++evaluations == 3, "evaluation {}", evaluations));

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
        REQUIRE(duplicate.error().code == Sora::ErrorCode::InvalidState);
    }

    auto reinstalled = Sora::CrashHandler::Install();
    REQUIRE(reinstalled.has_value());
}

TEST_CASE("Debugger detection is a side-effect-free query", "[Sora.Core.Diagnostics]") {
    [[maybe_unused]] const bool attached = Sora::Debug::IsDebuggerPresent();
    SUCCEED();
}
