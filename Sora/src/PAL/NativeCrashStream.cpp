/**
 * @file NativeCrashStream.cpp
 * @brief Native emergency-file ownership and complete allocation-free crash writes.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/NativeCrashStream.h>

#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace Sora::PAL {

    static_assert(std::is_trivially_copyable_v<NativeCrashStream>);
    static_assert(sizeof(NativeCrashStream) <= 2 * sizeof(void*));

    NativeCrashStream::operator bool() const noexcept {
        return static_cast<bool>(file_);
    }

    bool NativeCrashStream::Write(std::string_view text) const noexcept {
        return file_.Write(text);
    }

    bool NativeCrashStream::WriteHex(uintptr_t value) const noexcept {
        constexpr std::string_view digits = "0123456789ABCDEF";
        std::array<char, 2 + 2 * sizeof(uintptr_t)> output{};
        size_t position = output.size();
        do {
            output[--position] = digits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        output[--position] = 'x';
        output[--position] = '0';
        return Write({output.data() + position, output.size() - position});
    }

    bool NativeCrashStream::WriteUnsigned(uint64_t value) const noexcept {
        std::array<char, std::numeric_limits<uint64_t>::digits10 + 1> output{};
        size_t position = output.size();
        do {
            output[--position] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        return Write({output.data() + position, output.size() - position});
    }

    void NativeCrashStream::Flush() const noexcept {
        file_.Flush();
    }

    OwnedNativeCrashStream::~OwnedNativeCrashStream() = default;

    OwnedNativeCrashStream::OwnedNativeCrashStream(OwnedNativeCrashStream&& other) noexcept = default;

    OwnedNativeCrashStream& OwnedNativeCrashStream::operator=(OwnedNativeCrashStream&& other) noexcept = default;

    OwnedNativeCrashStream OwnedNativeCrashStream::OpenTruncated(const std::filesystem::path& path) noexcept {
        auto opened = File::Open(path, FileOpenOptions{
                                           .access = FileAccess::Write,
                                           .creation = FileCreation::CreateAlways,
                                           .share = FileShare::Read | FileShare::Delete,
                                           .flags = FileOpenFlag::None,
                                       });
        return opened ? OwnedNativeCrashStream{std::move(*opened)} : OwnedNativeCrashStream{};
    }

    NativeCrashStream NativeStandardErrorStream() noexcept {
        return NativeCrashStream{NativeStandardErrorFile()};
    }

} // namespace Sora::PAL
