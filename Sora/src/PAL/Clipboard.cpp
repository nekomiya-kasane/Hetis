/**
 * @file Clipboard.cpp
 * @brief Implement native process-global clipboard text access.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Clipboard.h>
#include <Sora/Core/PAL/GlobalMemory.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

namespace Sora::PAL::Clipboard {

    namespace {

#ifdef PLATFORM_WINDOWS
        /** @brief Exclusive Win32 clipboard session with explicit success-path close checking. */
        class ClipboardSession {
        public:
            ClipboardSession() noexcept = default;
            ClipboardSession(const ClipboardSession&) = delete;
            ClipboardSession& operator=(const ClipboardSession&) = delete;

            ClipboardSession(ClipboardSession&& other) noexcept : open_{std::exchange(other.open_, false)} {}

            ClipboardSession& operator=(ClipboardSession&& other) noexcept {
                if (this != &other) {
                    CloseIgnoringError();
                    open_ = std::exchange(other.open_, false);
                }
                return *this;
            }

            ~ClipboardSession() { CloseIgnoringError(); }

            /** @brief Open the clipboard with bounded backoff for ordinary cross-process contention. */
            [[nodiscard]] static Result<ClipboardSession> Open() {
                const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                if (!EnsureSystemAPIs(api.openClipboard, api.closeClipboard)) {
                    return std::unexpected(ErrorCode::NotSupported);
                }

                using namespace std::chrono_literals;

                constexpr std::chrono::milliseconds retryDelays[] = {0ms, 1ms, 2ms, 4ms};
                for (auto delay : retryDelays) {
                    if (delay != 0ms) {
                        std::this_thread::sleep_for(delay);
                    }

                    if (api.openClipboard(nullptr) != 0) {
                        ClipboardSession session;
                        session.open_ = true;
                        return session;
                    }
                }
                return std::unexpected(ErrorCode::ClipboardBusy);
            }

            /** @brief Close the session and report a native close failure. */
            [[nodiscard]] VoidResult Close() noexcept {
                if (!open_) {
                    return {};
                }
                const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                if (!EnsureSystemAPIs(api.closeClipboard) || api.closeClipboard() == 0) {
                    return std::unexpected(ErrorCode::ClipboardNativeFailure);
                }
                open_ = false;
                return {};
            }

        private:
            void CloseIgnoringError() noexcept {
                if (!open_) {
                    return;
                }

                open_ = false;
                if (const ClipboardSystemAPI& api = LoadClipboardSystemAPI(); EnsureSystemAPIs(api.closeClipboard)) {
                    api.closeClipboard();
                }
            }

            bool open_ = false;
        };

#endif

    } // namespace

    Result<std::optional<std::string>> ReadText() {
#ifdef PLATFORM_WINDOWS
        const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
        if (!EnsureSystemAPIs(api.isClipboardFormatAvailable, api.getClipboardData)) {
            return std::unexpected(ErrorCode::NotSupported);
        }

        // 1. Acquire the process-global clipboard and handle the absent-format case explicitly.
        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);

        if (!api.isClipboardFormatAvailable(WindowsSystem::kUnicodeTextClipboardFormat)) {
            if (auto closed = session.Close(); !closed) {
                return std::unexpected(closed.error());
            }
            return std::optional<std::string>{};
        }

        // 2. Validate the clipboard-owned allocation before exposing its contents.
        const GlobalMemoryView memory = GlobalMemoryView::Borrow(
            static_cast<GlobalMemoryHandle>(api.getClipboardData(WindowsSystem::kUnicodeTextClipboardFormat)));
        if (!memory) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        auto byteSize = memory.Size();
        if (!byteSize) {
            return std::unexpected(byteSize.error());
        }
        if (*byteSize == 0 || *byteSize % sizeof(wchar_t) != 0) {
            return std::unexpected(ErrorCode::InvalidClipboardData);
        }

        // 3. Lock, bound the terminator search by allocation size, and convert the exact text payload.
        auto locked = memory.Lock();
        if (!locked) {
            return std::unexpected(locked.error());
        }
        const auto* begin = reinterpret_cast<const wchar_t*>(locked->Data());
        const size_t capacity = *byteSize / sizeof(wchar_t);
        const auto* terminator = std::find(begin, begin + capacity, L'\0');
        if (terminator == begin + capacity) {
            return std::unexpected(ErrorCode::InvalidClipboardData);
        }
        auto text = Unicode::WideToUtf8(std::wstring_view{begin, static_cast<size_t>(terminator - begin)});
        if (!text) {
            return std::unexpected(text.error());
        }

        // 4. Report explicit release failures on the success path.
        if (auto unlocked = locked->Unlock(); !unlocked) {
            return std::unexpected(unlocked.error());
        }
        if (auto closed = session.Close(); !closed) {
            return std::unexpected(closed.error());
        }
        return std::optional<std::string>{std::move(*text)};
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    VoidResult WriteText(std::string_view text) {
#ifdef PLATFORM_WINDOWS
        static_assert(sizeof(wchar_t) == sizeof(uint16_t));
        const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
        if (!EnsureSystemAPIs(api.emptyClipboard, api.setClipboardData)) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        // 1. Convert and validate the complete native allocation size before allocating.
        auto wide = Unicode::Utf8ToWide(text);
        if (!wide) {
            return std::unexpected(wide.error());
        }
        if (wide->size() == std::numeric_limits<size_t>::max() ||
            wide->size() + 1 > std::numeric_limits<WindowsSystem::Size>::max() / sizeof(wchar_t)) {
            return std::unexpected(ErrorCode::ClipboardTextTooLarge);
        }

        // 2. Populate a movable allocation while retaining cleanup ownership locally.
        const WindowsSystem::Size byteSize = (wide->size() + 1) * sizeof(wchar_t);
        auto allocated = OwnedGlobalMemory::Allocate(byteSize);
        if (!allocated) {
            return std::unexpected{allocated.error()};
        }
        OwnedGlobalMemory memory = std::move(*allocated);
        auto locked = memory.Lock();
        if (!locked) {
            return std::unexpected(locked.error());
        }
        std::memcpy(locked->Data(), wide->data(), wide->size() * sizeof(wchar_t));
        reinterpret_cast<wchar_t*>(locked->Data())[wide->size()] = L'\0';
        if (auto unlocked = locked->Unlock(); !unlocked) {
            return std::unexpected(unlocked.error());
        }

        // 3. Publish under an exclusive session, transferring allocation ownership only after success.
        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);
        if (api.emptyClipboard() == 0) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        if (api.setClipboardData(WindowsSystem::kUnicodeTextClipboardFormat, memory.NativeHandle()) == nullptr) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        static_cast<void>(memory.Release());
        return session.Close();
#else
        static_cast<void>(text);
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    Result<bool> HasText() {
#ifdef PLATFORM_WINDOWS
        const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
        if (api.isClipboardFormatAvailable == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);
        const bool available = api.isClipboardFormatAvailable(WindowsSystem::kUnicodeTextClipboardFormat) != 0;
        if (auto closed = session.Close(); !closed) {
            return std::unexpected(closed.error());
        }
        return available;
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

    VoidResult Clear() {
#ifdef PLATFORM_WINDOWS
        const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
        if (api.emptyClipboard == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);
        if (api.emptyClipboard() == 0) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        return session.Close();
#else
        return std::unexpected(ErrorCode::NotSupported);
#endif
    }

} // namespace Sora::PAL::Clipboard
