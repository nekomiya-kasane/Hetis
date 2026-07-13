/**
 * @file NativeError.h
 * @brief Preserve operating-system error codes with their native standard-library category and diagnostic message.
 * @details @ref Sora::PAL::NativeError complements the portable @ref Sora::ErrorCode vocabulary. Native errors retain
 * platform-specific diagnostic identity, while higher-level services decide which portable semantic error applies in
 * their own context. Capture functions never allocate; message materialization and exception construction allocate only
 * when explicitly requested.
 * @ingroup PAL
 */
#pragma once

#include <string>
#include <string_view>
#include <system_error>

namespace Sora::PAL {

    /** @brief Value object retaining a native error value and its @c std::error_category. */
    class NativeError {
    public:
        /** @brief Construct an empty native error. */
        NativeError() noexcept = default;

        /**
         * @brief Construct from an existing category-bearing standard error code.
         * @param[in] code Native or generic standard error code.
         */
        explicit NativeError(std::error_code code) noexcept : code_{code} {}

        /** @brief Construct an error in @c std::generic_category, suitable for POSIX @c errno values. */
        [[nodiscard]] static NativeError FromErrno(int value) noexcept;

        /** @brief Construct an error in @c std::system_category, suitable for host operating-system error values. */
        [[nodiscard]] static NativeError FromSystem(int value) noexcept;

        /** @brief Return whether this object represents a nonzero error. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(code_); }

        /** @brief Return the category-bearing standard error code. */
        [[nodiscard]] const std::error_code& Code() const noexcept { return code_; }

        /** @brief Return the native integer value stored by the standard error code. */
        [[nodiscard]] int Value() const noexcept { return code_.value(); }

        /** @brief Return the standard category that interprets @ref Value. */
        [[nodiscard]] const std::error_category& Category() const noexcept { return code_.category(); }

        /** @brief Return the stable name of the standard error category. */
        [[nodiscard]] std::string_view CategoryName() const noexcept { return code_.category().name(); }

        /** @brief Materialize a normalized diagnostic message, encoded as UTF-8 on Windows. */
        [[nodiscard]] std::string Message() const;

        /** @brief Return a diagnostic containing the normalized message, category name, and native integer value. */
        [[nodiscard]] std::string ToString() const;

        friend bool operator==(const NativeError&, const NativeError&) noexcept = default;

    private:
        std::error_code code_{};
    };

    /**
     * @brief Capture the calling thread's current native error slot without allocating.
     * @return Win32 @c GetLastError on Windows, or POSIX @c errno on Unix-like targets.
     * @warning Capture immediately after an API reports failure; successful APIs need not clear a previous error value.
     */
    [[nodiscard]] NativeError CaptureLastNativeError() noexcept;

    /**
     * @brief Construct @c std::system_error from @p error and optional operation context.
     * @param[in] error Native error preserved from the failing operation.
     * @param[in] context Description of the operation that failed, excluding the OS-generated message.
     */
    [[nodiscard]] std::system_error MakeSystemError(NativeError error, std::string_view context = {});

} // namespace Sora::PAL
