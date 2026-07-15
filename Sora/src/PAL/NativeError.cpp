/**
 * @file NativeError.cpp
 * @brief Implement native error capture and normalized diagnostic message materialization.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/NativeError.h>
#include <Sora/Core/StringUtils.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <cerrno>
#include <format>
#include <memory>
#include <string>

#ifdef PLATFORM_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

namespace Sora::PAL {

    namespace {

#ifdef PLATFORM_WINDOWS
        /** @brief Release storage allocated by @c FormatMessageW. */
        struct LocalMemoryDeleter {
            void operator()(wchar_t* memory) const noexcept {
                if (memory != nullptr) {
                    ::LocalFree(memory);
                }
            }
        };

        /** @brief Return a UTF-8 message for one Win32 system-category error. */
        [[nodiscard]] std::string WindowsMessage(int value) {
            wchar_t* buffer = nullptr;
            constexpr DWORD flags =
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            const DWORD size = ::FormatMessageW(flags, nullptr, static_cast<DWORD>(value), 0,
                                                reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            if (size == 0 || buffer == nullptr) {
                return {};
            }
            const std::unique_ptr<wchar_t, LocalMemoryDeleter> storage{buffer};
            std::wstring_view wide{storage.get(), size};
            auto converted = Unicode::WideToUtf8<Unicode::InvalidSequencePolicy::Replace>(wide);
            std::string message = std::move(*converted);
            message.resize(Ascii::TrimEnd(message).size());
            return message;
        }
#endif

    } // namespace

    NativeError NativeError::FromErrno(int value) noexcept {
        return NativeError{std::error_code{value, std::generic_category()}};
    }

    NativeError NativeError::FromSystem(int value) noexcept {
        return NativeError{std::error_code{value, std::system_category()}};
    }

    std::string NativeError::Message() const {
        if (!code_) {
            return "Success";
        }

#ifdef PLATFORM_WINDOWS
        if (&code_.category() == &std::system_category()) {
            if (std::string message = WindowsMessage(code_.value()); !message.empty()) {
                return message;
            }
        }
#endif

        std::string message = code_.message();
        message.resize(Ascii::TrimEnd(message).size());
        return message;
    }

    std::string NativeError::ToString() const {
        if (!code_) {
            return "Success";
        }
        return std::format("{}[{}:{}]", Message(), CategoryName(), Value());
    }

    NativeError CaptureLastNativeError() noexcept {
#ifdef PLATFORM_WINDOWS
        return NativeError::FromSystem(static_cast<int>(::GetLastError()));
#else
        return NativeError::FromErrno(errno);
#endif
    }

    std::system_error MakeSystemError(NativeError error, std::string_view context) {
        if (context.empty()) {
            return std::system_error{error.Code()};
        }
        return std::system_error{error.Code(), std::string{context}};
    }

} // namespace Sora::PAL
