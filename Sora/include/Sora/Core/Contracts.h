/**
 * @file Contracts.h
 * @brief Portable C++26 contract vocabulary with a strong runtime fallback.
 * @ingroup Core
 */
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <source_location>
#include <string_view>
#include <utility>

#if defined(__cpp_contracts) && __cpp_contracts >= 202502L
#    define SORA_HAS_NATIVE_CONTRACTS 1
#else
#    define SORA_HAS_NATIVE_CONTRACTS 0
#endif

namespace Sora::Contracts {

    /** @brief C++26 contract assertion categories. */
    enum class Kind : uint8_t {
        Precondition,
        Postcondition,
        Assertion,
    };

    /** @brief C++26 evaluation semantics for a contract assertion. */
    enum class EvaluationSemantic : uint8_t {
        Ignore,
        Observe,
        Enforce,
        QuickEnforce,
    };

    /** @brief C++26 modes by which a contract violation can be detected. */
    enum class DetectionMode : uint8_t {
        PredicateFalse,
        EvaluationException,
    };

    /** @brief Portable materialized contract violation passed to the fallback handler. */
    struct Violation {
        std::string_view condition;
        Kind kind = Kind::Assertion;
        EvaluationSemantic semantic = EvaluationSemantic::Enforce;
        DetectionMode detection = DetectionMode::PredicateFalse;
        std::source_location source = std::source_location::current();
        bool terminating = true;
    };

    /** @brief Non-owning contract violation handler. */
    using ViolationHandler = void (*)(const Violation&) noexcept;

    namespace Detail {

        inline std::atomic<ViolationHandler>& HandlerStorage() noexcept {
            static std::atomic<ViolationHandler> handler = nullptr;
            return handler;
        }

        /** @brief Evaluate one fallback predicate without evaluating it more than once. */
        template<typename Predicate>
        [[nodiscard]] constexpr bool Evaluate(Predicate&& predicate, std::string_view condition, Kind kind,
                                              EvaluationSemantic semantic,
                                              std::source_location source = std::source_location::current()) {
            if (semantic == EvaluationSemantic::Ignore) {
                return true;
            }

            bool result = false;
            DetectionMode detection = DetectionMode::PredicateFalse;
            try {
                result = static_cast<bool>(std::forward<Predicate>(predicate)());
            } catch (...) {
                detection = DetectionMode::EvaluationException;
            }
            if (result) {
                return true;
            }

            if consteval {
                throw "Sora contract violation";
            } else {
                const bool terminating =
                    semantic == EvaluationSemantic::Enforce || semantic == EvaluationSemantic::QuickEnforce;
                if (semantic != EvaluationSemantic::QuickEnforce) {
                    const Violation violation{.condition = condition,
                                              .kind = kind,
                                              .semantic = semantic,
                                              .detection = detection,
                                              .source = source,
                                              .terminating = terminating};
                    if (ViolationHandler handler = HandlerStorage().load(std::memory_order_acquire);
                        handler != nullptr) {
                        handler(violation);
                    }
                    std::fprintf(stderr, "Sora contract violation at %s:%u: ", source.file_name(), source.line());
                    std::fwrite(condition.data(), 1, condition.size(), stderr);
                    std::fputc('\n', stderr);
                }
                if (!terminating) {
                    return false;
                }
                std::terminate();
            }
        }

        /** @brief Evaluate a portable contract assertion with an explicit semantic. */
        template<typename Predicate>
        [[nodiscard]] constexpr bool EvaluateContract(Predicate&& predicate, std::string_view condition, Kind kind,
                                                      EvaluationSemantic semantic,
                                                      std::source_location source = std::source_location::current()) {
            return Evaluate(std::forward<Predicate>(predicate), condition, kind, semantic, source);
        }

    } // namespace Detail

    /** @brief Replace the fallback contract handler and return the previous handler. */
    [[nodiscard]] inline ViolationHandler SetViolationHandler(ViolationHandler handler) noexcept {
        return Detail::HandlerStorage().exchange(handler, std::memory_order_acq_rel);
    }

    /** @brief Return the currently configured fallback contract handler. */
    [[nodiscard]] inline ViolationHandler GetViolationHandler() noexcept {
        return Detail::HandlerStorage().load(std::memory_order_acquire);
    }

} // namespace Sora::Contracts

/** @brief Attach a native C++26 function precondition when available. */
/** @brief Evaluate an assertion as a non-nodiscard Boolean expression. */
// clang-format off
#if SORA_HAS_NATIVE_CONTRACTS
#    define SORA_FUNCTION_PRE(condition) pre(condition)
#    define SORA_FUNCTION_POST(result, condition) post(result : condition)
#    define SORA_ASSERT(condition) contract_assert(condition)
#    define SORA_CONTRACT_ASSERT(condition) contract_assert(condition)
#    define SORA_ASSERT_VALUE(condition)                                                                  \
        [&]() -> bool {                                                                                   \
            const bool result = static_cast<bool>(condition);                                            \
            contract_assert(result);                                                                      \
            return result;                                                                                \
        }()
#else
#    define SORA_FUNCTION_PRE(condition)
#    define SORA_FUNCTION_POST(result, condition)
#    define SORA_ASSERT(condition)                                                                        \
        static_cast<void>(::Sora::Contracts::Detail::EvaluateContract(                                  \
            [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,                \
            ::Sora::Contracts::Kind::Assertion, ::Sora::Contracts::EvaluationSemantic::Enforce,          \
            std::source_location::current()))
#    define SORA_CONTRACT_ASSERT(condition) SORA_ASSERT(condition)
#    define SORA_ASSERT_VALUE(condition)                                                                  \
        [&]() -> bool {                                                                                   \
            return ::Sora::Contracts::Detail::EvaluateContract(                                           \
                [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,             \
                ::Sora::Contracts::Kind::Assertion, ::Sora::Contracts::EvaluationSemantic::Enforce,       \
                std::source_location::current());                                                        \
        }()
#endif

/* The declaration forms are deliberately separate from the statement forms below. Native pre/post specifiers can
 * only appear in a function declarator; the fallback cannot inject a check at function entry/exit from a macro that
 * expands at the declaration site. Portable code therefore uses SORA_PRE/SORA_POST in the function body when the
 * compiler does not provide P2900. */

/** @brief Portable body-level precondition with strong runtime enforcement when native Contracts are unavailable. */
#define SORA_PRE(condition)                                                                               \
    static_cast<void>(::Sora::Contracts::Detail::EvaluateContract(                                        \
        [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,                    \
        ::Sora::Contracts::Kind::Precondition, ::Sora::Contracts::EvaluationSemantic::Enforce,            \
        std::source_location::current()))

/** @brief Portable body-level postcondition with strong runtime enforcement when native Contracts are unavailable. */
#define SORA_POST(condition)                                                                              \
    static_cast<void>(::Sora::Contracts::Detail::EvaluateContract(                                        \
        [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,                    \
        ::Sora::Contracts::Kind::Postcondition, ::Sora::Contracts::EvaluationSemantic::Enforce,           \
        std::source_location::current()))

/** @brief Portable postcondition spelling that documents the result binding used by the predicate. */
#define SORA_POST_VALUE(result, condition) SORA_POST(condition)

/** @brief Evaluate a contract assertion with the observe semantic. */
#define SORA_OBSERVE(condition)                                                                            \
    ::Sora::Contracts::Detail::EvaluateContract(                                                         \
        [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,                    \
        ::Sora::Contracts::Kind::Assertion, ::Sora::Contracts::EvaluationSemantic::Observe,              \
        std::source_location::current())

/** @brief Evaluate a contract assertion with the enforce semantic. */
#define SORA_ENFORCE(condition) SORA_ASSERT(condition)

/** @brief Evaluate a contract assertion with the quick-enforce semantic. */
#define SORA_QUICK_ENFORCE(condition)                                                                      \
    static_cast<void>(::Sora::Contracts::Detail::EvaluateContract(                                        \
        [&]() constexpr -> bool { return static_cast<bool>(condition); }, #condition,                    \
        ::Sora::Contracts::Kind::Assertion, ::Sora::Contracts::EvaluationSemantic::QuickEnforce,         \
        std::source_location::current()))

/** @brief Evaluate no predicate and model the C++26 ignore semantic. */
#define SORA_IGNORE(condition) static_cast<void>(sizeof(condition))

/** @brief Evaluate a condition as an optimizer assumption without a runtime check. */
#if defined(__has_builtin)
#    if __has_builtin(__builtin_assume)
#        define SORA_ASSUME(condition) __builtin_assume(static_cast<bool>(condition))
#    endif
#endif
#ifndef SORA_ASSUME
#    define SORA_ASSUME(condition) static_cast<void>(sizeof(condition))
#endif

/** @brief Optional audit contract; it remains checked unless explicitly disabled by the build. */
#ifndef SORA_DISABLE_AUDIT_CONTRACTS
#    define SORA_AUDIT(condition) SORA_ASSERT(condition)
#else
#    define SORA_AUDIT(condition) SORA_ASSUME(condition)
#endif

/* Short aliases are provided only when the host program has not claimed these conventional macro names. */
#ifndef PRE
#    define PRE(condition) SORA_PRE(condition)
#endif
#ifndef POST
#    define POST(condition) SORA_POST(condition)
#endif
#ifndef CONTRACT_ASSERT
#    define CONTRACT_ASSERT(condition) SORA_CONTRACT_ASSERT(condition)
#endif
#ifndef ASSERT
#    define ASSERT(condition) SORA_ASSERT_VALUE(condition)
#endif
#ifndef AUDIT_ASSERT
#    define AUDIT_ASSERT(condition) SORA_AUDIT(condition)
#endif
#ifndef ASSUME_ASSERT
#    define ASSUME_ASSERT(condition) SORA_ASSUME(condition)
#endif
// clang-format on
