/**
 * @file Assertion.h
 * @brief Synchronous invariant and verification failure reporting with configurable terminal actions.
 * @ingroup Core
 */
#pragma once

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace Sora {

    class StackTrace;

    /** @brief Semantic class of a failed runtime condition. */
    enum class AssertionKind : uint8_t {
        Assertion,   /**< Debug-build invariant checked by @ref Assert. */
        Verification /**< Always-evaluated condition checked by @ref Verify. */
    };

    /** @brief Terminal action performed after an assertion failure has been reported. */
    enum class AssertionAction : uint8_t {
        Continue,                      /**< Return after reporting. Intended for tests and nonfatal diagnostics. */
        Break,                         /**< Raise a breakpoint and continue if the trap is resumed. */
        BreakIfDebuggerOtherwiseAbort, /**< Break under a debugger; abort when no debugger is attached. */
        Abort                          /**< Terminate the process with @c std::abort. */
    };

    /** @brief Process-wide assertion behavior. */
    struct AssertionSettings {
        AssertionAction action = AssertionAction::BreakIfDebuggerOtherwiseAbort; /**< Post-report terminal action. */
        bool captureStackTrace = true; /**< Capture a resolved stack trace before invoking the reporter. */

        friend bool operator==(const AssertionSettings&, const AssertionSettings&) noexcept = default;
    };

    /** @brief Fully materialized synchronous assertion diagnostic passed to the active reporter. */
    struct AssertionFailure {
        AssertionKind kind = AssertionKind::Assertion; /**< Failed condition category. */
        std::string_view condition;                    /**< Stringized source expression. */
        std::string message;                           /**< Optional formatted diagnostic payload. */
        std::source_location source;                   /**< Call site at which the condition was evaluated. */
        const StackTrace* stackTrace = nullptr;         /**< Captured stack, valid only during the reporter call. */
    };

    /** @brief Non-owning synchronous reporter invoked on the failing thread. */
    using AssertionReporter = void (*)(const AssertionFailure&) noexcept;

    /** @brief Atomically replace the process-wide assertion settings and return the previous value. */
    [[nodiscard]] AssertionSettings SetAssertionSettings(AssertionSettings settings) noexcept;

    /** @brief Return a coherent snapshot of the process-wide assertion settings. */
    [[nodiscard]] AssertionSettings GetAssertionSettings() noexcept;

    /**
     * @brief Atomically replace the process-wide assertion reporter and return the previous reporter.
     * @param[in] reporter Synchronous reporter, or @c nullptr to restore Sora's default stderr reporter.
     * @warning The reporter must be thread-safe, must not throw, and must not retain references into the diagnostic.
     */
    [[nodiscard]] AssertionReporter SetAssertionReporter(AssertionReporter reporter) noexcept;

    /** @brief Return the configured reporter, or @c nullptr when Sora's default reporter is active. */
    [[nodiscard]] AssertionReporter GetAssertionReporter() noexcept;

    /** @cond INTERNAL */
    namespace Detail {

        /** @brief Boolean condition paired with the source location at which its implicit conversion occurs. */
        struct LocatedCondition {
            bool value = false;            /**< Evaluated condition value. */
            std::source_location source{}; /**< User call site. */

            /** @brief Capture @p condition and its caller location. */
            constexpr LocatedCondition(bool condition,
                                       std::source_location location = std::source_location::current()) noexcept
                : value(condition), source(location) {}
        };

        /** @brief Materialize, report, and apply policy to one failed condition. */
        void ReportAssertionFailure(AssertionKind kind, std::string_view condition, std::string message,
                                    std::source_location source) noexcept;

        /** @brief Evaluate an unformatted condition and report failure when false. */
        [[nodiscard]] inline bool EvaluateCondition(bool value, AssertionKind kind, std::string_view condition,
                                                    std::source_location source) noexcept {
            if (!value) {
                ReportAssertionFailure(kind, condition, {}, source);
            }
            return value;
        }

        /** @brief Evaluate a condition and format its diagnostic payload only on failure. */
        template<typename... Args>
        [[nodiscard]] bool EvaluateCondition(bool value, AssertionKind kind, std::string_view condition,
                                             std::source_location source, std::format_string<Args...> format,
                                             Args&&... args) noexcept {
            if (value) {
                return true;
            }

            try {
                ReportAssertionFailure(kind, condition, std::format(format, std::forward<Args>(args)...), source);
            } catch (...) {
                ReportAssertionFailure(kind, condition, "<assertion message formatting failed>", source);
            }
            return false;
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Report a failed invariant only in assertion-enabled builds.
     * @param condition Contextually boolean expression evaluated exactly once in every build.
     * @note This function API always evaluates @p condition before entry; only failure reporting is compiled out.
     */
    template<typename... Args>
    inline void Assert(Detail::LocatedCondition condition, std::format_string<Args...> format = "",
                       Args&&... args) noexcept {
#if !defined(NDEBUG) || defined(SORA_ENABLE_ASSERTIONS)
        static_cast<void>(::Sora::Detail::EvaluateCondition(condition.value, ::Sora::AssertionKind::Assertion, "",
                                                            condition.source, format,
                                                            std::forward<Args>(args)...));
#else
        static_cast<void>(condition);
#endif
    }

    /**
     * @brief Evaluate @p condition in every build, report failure, and return its boolean value.
     * @param condition Contextually boolean expression evaluated exactly once.
     */
    template<typename... Args>
    [[nodiscard]] inline bool Verify(Detail::LocatedCondition condition, std::format_string<Args...> format = "",
                                     Args&&... args) noexcept {
        return ::Sora::Detail::EvaluateCondition(condition.value, ::Sora::AssertionKind::Verification, "",
                                                 condition.source, format, std::forward<Args>(args)...);
    }

} // namespace Sora

using Sora::Assert;
using Sora::Verify;
