/**
 * @file NativeCrashStream.h
 * @brief Preopened, allocation-free native output for corrupted-process diagnostics.
 * @ingroup PAL
 *
 * @details Use @ref OwnedNativeCrashStream only during normal startup to open an emergency file, then pass its
 * non-owning @ref NativeCrashStream view into a crash runtime. Creating the owner initializes the process-lifetime
 * native function table before the view is published. The view itself stores only the handle, so
 * @ref NativeCrashStream::Write and @ref NativeCrashStream::Flush perform no allocation, dynamic loading, path lookup,
 * locale processing, or C++ stream buffering.
 *
 * @code{.cpp}
 * auto emergency = Sora::PAL::OwnedNativeCrashStream::OpenTruncated("crash-emergency.txt");
 * if (emergency) {
 *     Sora::PAL::NativeCrashStream stream = emergency.View();
 *     static_cast<void>(stream.Write("fatal state\n"));
 *     stream.Flush();
 * }
 * @endcode
 *
 * @warning Opening files is not crash-safe. Complete all ownership and path operations before installing a native
 * crash handler. A view must not outlive its owning stream.
 */
#pragma once

#include <Sora/Core/PAL/File.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

namespace Sora::PAL {

    /** @brief Non-owning crash-safe writer over a preopened native output handle. */
    class NativeCrashStream {
    public:
        /** @brief Construct an invalid stream. */
        constexpr NativeCrashStream() noexcept = default;

        /** @brief Return whether this view has a valid native handle. */
        [[nodiscard]] explicit operator bool() const noexcept;

        /** @brief Write every byte in @p text, retrying interruptible and partial native writes. */
        [[nodiscard]] bool Write(std::string_view text) const noexcept;

        /** @brief Write @p value in uppercase hexadecimal notation with a @c 0x prefix. */
        [[nodiscard]] bool WriteHex(uintptr_t value) const noexcept;

        /** @brief Write @p value in decimal notation. */
        [[nodiscard]] bool WriteUnsigned(uint64_t value) const noexcept;

        /** @brief Best-effort synchronization of native buffers with the underlying device. */
        void Flush() const noexcept;

        friend constexpr bool operator==(NativeCrashStream, NativeCrashStream) noexcept = default;

    private:
        friend class OwnedNativeCrashStream;
        friend NativeCrashStream NativeStandardErrorStream() noexcept;

        explicit constexpr NativeCrashStream(BorrowedFile file) noexcept : file_{file} {}

        BorrowedFile file_{};
    };

    /** @brief Move-only owner for a native emergency file opened before crash-handler installation. */
    class OwnedNativeCrashStream {
    public:
        /** @brief Construct without an owned native stream. */
        constexpr OwnedNativeCrashStream() noexcept = default;

        /** @brief Close the owned native stream when valid. */
        ~OwnedNativeCrashStream();

        OwnedNativeCrashStream(const OwnedNativeCrashStream&) = delete;
        OwnedNativeCrashStream& operator=(const OwnedNativeCrashStream&) = delete;

        /** @brief Transfer native-stream ownership from @p other. */
        OwnedNativeCrashStream(OwnedNativeCrashStream&& other) noexcept;

        /** @brief Close the current stream, then transfer ownership from @p other. */
        OwnedNativeCrashStream& operator=(OwnedNativeCrashStream&& other) noexcept;

        /** @brief Open or replace @p path for native crash output without C++ stream state. */
        [[nodiscard]] static OwnedNativeCrashStream OpenTruncated(const std::filesystem::path& path) noexcept;

        /** @brief Return whether this object owns a valid native stream. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(file_); }

        /** @brief Return a non-owning view valid until this owner is reset, moved, or destroyed. */
        [[nodiscard]] NativeCrashStream View() const noexcept { return NativeCrashStream{file_.Borrow()}; }

    private:
        explicit OwnedNativeCrashStream(File file) noexcept : file_{std::move(file)} {}

        File file_{};
    };

    /** @brief Return a crash-safe view of the process standard-error stream using pre-resolved native entry points. */
    [[nodiscard]] NativeCrashStream NativeStandardErrorStream() noexcept;

} // namespace Sora::PAL
