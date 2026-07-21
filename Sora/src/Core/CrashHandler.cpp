/**
 * @file CrashHandler.cpp
 * @brief Allocation-free native crash record and emergency-stream implementation.
 * @details Install and retain @ref Sora::CrashHandler as documented in @ref Sora::CrashHandlerOptions. This source
 * deliberately does not call stack symbolization, logging, formatting, allocation, or locking from a fatal handler.
 * @ingroup Core
 */

#include "Sora/Core/CrashHandler.h"
#include "Sora/Core/PAL/SystemAPI.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace Sora {

    namespace {

        struct CrashRuntime {
            CrashStream standardError{};
            CrashStream emergencyStream{};
            CrashCallback callback = nullptr;
            bool mirrorToStandardError = true;
        };

        std::atomic<bool> gCrashHandlerInstalled{false};

        /** @brief Fixed-capacity emergency text builder suitable for a corrupted-process path. */
        class EmergencyBuffer {
        public:
            void Append(std::string_view text) noexcept {
                const size_t count = std::min(storage_.size() - size_, text.size());
                for (size_t index = 0; index < count; ++index) {
                    storage_[size_ + index] = text[index];
                }
                size_ += count;
            }

            void AppendHex(uintptr_t value) noexcept {
                constexpr std::string_view digits = "0123456789ABCDEF";
                std::array<char, 2 * sizeof(uintptr_t)> reversed{};
                size_t count = 0;
                do {
                    reversed[count++] = digits[value & 0xF];
                    value >>= 4;
                } while (value != 0 && count < reversed.size());
                Append("0x");
                while (count != 0) {
                    const char digit = reversed[--count];
                    Append(std::string_view{&digit, 1});
                }
            }

            void AppendUnsigned(uint64_t value) noexcept {
                std::array<char, std::numeric_limits<uint64_t>::digits10 + 1> reversed{};
                size_t count = 0;
                do {
                    reversed[count++] = static_cast<char>('0' + value % 10);
                    value /= 10;
                } while (value != 0);
                while (count != 0) {
                    const char digit = reversed[--count];
                    Append(std::string_view{&digit, 1});
                }
            }

            [[nodiscard]] std::string_view View() const noexcept { return {storage_.data(), size_}; }

        private:
            std::array<char, 1024> storage_{};
            size_t size_ = 0;
        };

        void WriteRecord(const CrashContext& context, const CrashRuntime* runtime) noexcept {
            EmergencyBuffer output;
            output.Append("\n=== SORA CRASH RECORD ===\nReason: ");
            output.Append(context.reason);
            output.Append("\nCode: ");
            output.AppendUnsigned(context.code);
            output.Append("\nFault address: ");
            output.AppendHex(context.faultAddress);
            output.Append("\nInstruction pointer: ");
            output.AppendHex(context.instructionPointer);
            output.Append("\n");

            const CrashStream standardError = runtime != nullptr ? runtime->standardError : CrashStream{};
            if (runtime == nullptr || runtime->mirrorToStandardError) {
                [[maybe_unused]] const bool written = standardError.Write(output.View());
            }
            if (runtime != nullptr && runtime->emergencyStream && runtime->emergencyStream != standardError) {
                [[maybe_unused]] const bool written = runtime->emergencyStream.Write(output.View());
            }

            if (runtime != nullptr && runtime->callback != nullptr) {
                const CrashStream callbackStream = runtime->emergencyStream ? runtime->emergencyStream : standardError;
                runtime->callback(context, callbackStream);
            }

            constexpr std::string_view footer = "=== END SORA CRASH RECORD ===\n";
            if (runtime == nullptr || runtime->mirrorToStandardError) {
                [[maybe_unused]] const bool written = standardError.Write(footer);
                standardError.Flush();
            }
            if (runtime != nullptr && runtime->emergencyStream && runtime->emergencyStream != standardError) {
                [[maybe_unused]] const bool written = runtime->emergencyStream.Write(footer);
                runtime->emergencyStream.Flush();
            }
        }

        void HandleNativeCrash(const PAL::NativeCrashContext& nativeContext, void* userData) noexcept {
            const auto* runtime = static_cast<const CrashRuntime*>(userData);
            const CrashContext context{
                .reason = nativeContext.reason,
                .code = nativeContext.code,
                .faultAddress = nativeContext.faultAddress,
                .instructionPointer = nativeContext.instructionPointer,
            };
            WriteRecord(context, runtime);
        }

    } // namespace

    struct CrashHandler::State {
        CrashRuntime runtime{};
        PAL::OwnedNativeCrashStream emergencyStream{};
        bool nativeHandlerInstalled = false;

        ~State() noexcept {
            if (nativeHandlerInstalled) {
                PAL::UninstallNativeCrashHandler();
            }
            gCrashHandlerInstalled.store(false, std::memory_order_release);
        }
    };

    Result<CrashHandler> CrashHandler::Install() noexcept {
        return Install(CrashHandlerOptions{});
    }

    Result<CrashHandler> CrashHandler::Install(const CrashHandlerOptions& options) noexcept {
        bool expected = false;
        if (!gCrashHandlerInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return std::unexpected(ErrorCode::InvalidState);
        }

        std::unique_ptr<State> state;
        try {
            state = std::make_unique<State>();
        } catch (...) {
            gCrashHandlerInstalled.store(false, std::memory_order_release);
            return std::unexpected(ErrorCode::OutOfMemory);
        }

        // 1. Prepare immutable runtime state before publishing it to the native crash adapter.
        state->runtime.callback = options.callback;
        state->runtime.mirrorToStandardError = options.mirrorToStandardError;
        state->runtime.standardError = PAL::NativeStandardErrorStream();

        // 2. Acquire optional output storage before publishing the native handler.
        if (!options.emergencyFile.empty()) {
            state->emergencyStream = PAL::OwnedNativeCrashStream::OpenTruncated(options.emergencyFile);
            if (!state->emergencyStream) {
                return std::unexpected(ErrorCode::EmergencyPathInvalid);
            }
            state->runtime.emergencyStream = state->emergencyStream.View();
        }

        // 3. Publish the fully initialized runtime through the platform-normalized crash adapter.
        if (!PAL::InstallNativeCrashHandler(HandleNativeCrash, &state->runtime, options.suppressSystemErrorDialogs)) {
            return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
        }
        state->nativeHandlerInstalled = true;

        return CrashHandler{std::move(state)};
    }

    CrashHandler::CrashHandler(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    CrashHandler::~CrashHandler() = default;

    CrashHandler::CrashHandler(CrashHandler&& other) noexcept = default;

    CrashHandler& CrashHandler::operator=(CrashHandler&& other) noexcept = default;

} // namespace Sora
