/**
 * @file Guard.h
 * @brief Scope-exit guards selected by a compile-time trigger mode.
 * @ingroup Core
 *
 * @details @ref ScopeExit executes on every scope exit, @ref ScopeFail only while a new exception propagates, and
 * @ref ScopeSuccess only after successful completion. Guards own their callables and are move-only; call @c Release()
 * to disarm one. Exit and failure callbacks must not throw because they run from a non-throwing destructor. A success
 * callback may throw on a successful scope exit.
 *
 * @code{.cpp}
 * bool committed = false;
 * Sora::ScopeExit rollback{[&] noexcept {
 *     if (!committed) {
 *         RollbackTransaction();
 *     }
 * }};
 * CommitTransaction();
 * committed = true;
 * @endcode
 */
#pragma once

#include <Sora/Core/ABI/Runtime.h>
#include <Sora/Core/Traits/ConstructionTraits.h>
#include <Sora/Platform.h>

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @cond INTERNAL */
    namespace Detail {

        template<typename T>
        concept ScopeCallback = std::is_object_v<T> && std::invocable<T&>;

        enum class ScopeGuardMode : uint8_t { Exit, Failure, Success };

        template<ScopeGuardMode Mode>
        class ScopeGuardState {
            using State = std::conditional_t<Mode == ScopeGuardMode::Exit, bool, int>;

            [[nodiscard]] static constexpr State InitialState() noexcept {
                if constexpr (Mode == ScopeGuardMode::Exit) {
                    return true;
                } else {
                    return ABI::UncaughtExceptionCount();
                }
            }

        public:
            static constexpr bool kInvokeOnConstructionFailure = Mode != ScopeGuardMode::Success;

            [[nodiscard]] constexpr bool ShouldInvoke() const noexcept {
                if constexpr (Mode == ScopeGuardMode::Exit) {
                    return state_;
                } else if constexpr (Mode == ScopeGuardMode::Failure) {
                    return ABI::UncaughtExceptionCount() > state_;
                } else {
                    return ABI::UncaughtExceptionCount() <= state_;
                }
            }

            constexpr void Release() noexcept {
                if constexpr (Mode == ScopeGuardMode::Exit) {
                    state_ = false;
                } else if constexpr (Mode == ScopeGuardMode::Failure) {
                    state_ = std::numeric_limits<int>::max();
                } else {
                    state_ = -1;
                }
            }

        private:
            State state_ = InitialState();
        };

        template<ScopeGuardMode Mode, ScopeCallback ExitFunction>
        class ScopeGuard {
            using State = ScopeGuardState<Mode>;

        public:
            template<typename Function>
                requires Concept::SourcePreservingConstructible<ExitFunction, Function> &&
                         (!State::kInvokeOnConstructionFailure || std::invocable<Function&>)
            constexpr explicit ScopeGuard(Function&& function) noexcept(
                Concept::NothrowForwardConstructible<ExitFunction, Function>) try
                : exitFunction_{ForwardForConstruction<ExitFunction, Function>(function)} {
            } catch (...) {
                if constexpr (State::kInvokeOnConstructionFailure) {
                    std::invoke(function);
                }
                throw;
            }

            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;
            ScopeGuard& operator=(ScopeGuard&&) = delete;

            constexpr ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<ExitFunction> ||
                                                              std::is_nothrow_copy_constructible_v<ExitFunction>)
                requires(std::is_nothrow_move_constructible_v<ExitFunction> || std::copy_constructible<ExitFunction>)
                : exitFunction_{std::move_if_noexcept(other.exitFunction_)}, state_{other.state_} {
                other.Release();
            }

            constexpr ~ScopeGuard() noexcept(Mode != ScopeGuardMode::Success ||
                                             std::is_nothrow_invocable_v<ExitFunction&>) {
                if (state_.ShouldInvoke()) {
                    std::invoke(exitFunction_);
                }
            }

            constexpr void Release() noexcept { state_.Release(); }

        private:
            NO_UNIQUE_ADDRESS ExitFunction exitFunction_;
            NO_UNIQUE_ADDRESS State state_;
        };

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Execute a callable when the current scope exits unless explicitly released.
     * @tparam ExitFunction Callable object type invoked without arguments.
     */
    template<Detail::ScopeCallback ExitFunction>
    class [[nodiscard]] ScopeExit : public Detail::ScopeGuard<Detail::ScopeGuardMode::Exit, ExitFunction> {
    public:
        using Detail::ScopeGuard<Detail::ScopeGuardMode::Exit, ExitFunction>::ScopeGuard;
    };

    /** @brief Deduce the stored callback type by value. */
    template<typename ExitFunction>
    ScopeExit(ExitFunction) -> ScopeExit<ExitFunction>;

    /**
     * @brief Execute a callable only when scope exit propagates a new exception.
     * @tparam ExitFunction Callable object type invoked without arguments.
     */
    template<Detail::ScopeCallback ExitFunction>
    class [[nodiscard]] ScopeFail : public Detail::ScopeGuard<Detail::ScopeGuardMode::Failure, ExitFunction> {
    public:
        using Detail::ScopeGuard<Detail::ScopeGuardMode::Failure, ExitFunction>::ScopeGuard;
    };

    /** @brief Deduce the stored callback type by value. */
    template<typename ExitFunction>
    ScopeFail(ExitFunction) -> ScopeFail<ExitFunction>;

    /**
     * @brief Execute a callable only when scope exit does not propagate a new exception.
     * @tparam ExitFunction Callable object type invoked without arguments.
     */
    template<Detail::ScopeCallback ExitFunction>
    class [[nodiscard]] ScopeSuccess : public Detail::ScopeGuard<Detail::ScopeGuardMode::Success, ExitFunction> {
    public:
        using Detail::ScopeGuard<Detail::ScopeGuardMode::Success, ExitFunction>::ScopeGuard;
    };

    /** @brief Deduce the stored callback type by value. */
    template<typename ExitFunction>
    ScopeSuccess(ExitFunction) -> ScopeSuccess<ExitFunction>;

} // namespace Sora
