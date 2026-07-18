/**
 * @file Clipboard.cpp
 * @brief Implement native process-global clipboard text access.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Clipboard.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <cstring>
#include <limits>
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
                if (api.sleep == nullptr || api.openClipboard == nullptr || api.closeClipboard == nullptr) {
                    return std::unexpected(ErrorCode::NotSupported);
                }
                constexpr WindowsSystem::DWord retryDelaysMilliseconds[] = {0, 1, 2, 4};
                for (WindowsSystem::DWord delay : retryDelaysMilliseconds) {
                    if (delay != 0) {
                        api.sleep(delay);
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
                if (api.closeClipboard == nullptr || api.closeClipboard() == 0) {
                    return std::unexpected(ErrorCode::ClipboardNativeFailure);
                }
                open_ = false;
                return {};
            }

        private:
            void CloseIgnoringError() noexcept {
                if (open_) {
                    open_ = false;
                    const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                    if (api.closeClipboard != nullptr) {
                        api.closeClipboard();
                    }
                }
            }

            bool open_ = false;
        };

        /** @brief Movable global-memory allocation that frees storage until ownership transfers to the clipboard. */
        class OwnedGlobalMemory {
        public:
            explicit OwnedGlobalMemory(WindowsSystem::GlobalMemory handle) noexcept : handle_{handle} {}
            OwnedGlobalMemory(const OwnedGlobalMemory&) = delete;
            OwnedGlobalMemory& operator=(const OwnedGlobalMemory&) = delete;

            OwnedGlobalMemory(OwnedGlobalMemory&& other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}

            OwnedGlobalMemory& operator=(OwnedGlobalMemory&& other) noexcept {
                if (this != &other) {
                    Reset();
                    handle_ = std::exchange(other.handle_, nullptr);
                }
                return *this;
            }

            ~OwnedGlobalMemory() { Reset(); }

            [[nodiscard]] WindowsSystem::GlobalMemory Get() const noexcept { return handle_; }
            [[nodiscard]] WindowsSystem::GlobalMemory Release() noexcept { return std::exchange(handle_, nullptr); }

        private:
            void Reset() noexcept {
                if (handle_ != nullptr) {
                    const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                    if (api.globalFree != nullptr) {
                        api.globalFree(handle_);
                    }
                    handle_ = nullptr;
                }
            }

            WindowsSystem::GlobalMemory handle_ = nullptr;
        };

        /** @brief Locked view of global memory with checked explicit unlock and destructor fallback. */
        class GlobalMemoryLock {
        public:
            GlobalMemoryLock(const GlobalMemoryLock&) = delete;
            GlobalMemoryLock& operator=(const GlobalMemoryLock&) = delete;

            GlobalMemoryLock(GlobalMemoryLock&& other) noexcept
                : handle_{std::exchange(other.handle_, nullptr)}, data_{std::exchange(other.data_, nullptr)} {}

            GlobalMemoryLock& operator=(GlobalMemoryLock&& other) noexcept {
                if (this != &other) {
                    UnlockIgnoringError();
                    handle_ = std::exchange(other.handle_, nullptr);
                    data_ = std::exchange(other.data_, nullptr);
                }
                return *this;
            }

            ~GlobalMemoryLock() { UnlockIgnoringError(); }

            /** @brief Lock @p handle and report failure without retaining platform diagnostics. */
            [[nodiscard]] static Result<GlobalMemoryLock> Lock(WindowsSystem::GlobalMemory handle) {
                const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                if (api.globalLock == nullptr || api.globalUnlock == nullptr || api.setLastError == nullptr ||
                    api.getLastError == nullptr) {
                    return std::unexpected(ErrorCode::NotSupported);
                }
                void* data = api.globalLock(handle);
                if (data == nullptr) {
                    return std::unexpected(ErrorCode::ClipboardNativeFailure);
                }
                return GlobalMemoryLock{handle, LockedData{data}};
            }

            [[nodiscard]] void* Data() const noexcept { return data_; }

            /** @brief Unlock storage while distinguishing zero lock count from a native failure. */
            [[nodiscard]] VoidResult Unlock() noexcept {
                if (data_ == nullptr) {
                    return {};
                }
                const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                api.setLastError(WindowsSystem::kErrorSuccess);
                if (api.globalUnlock(handle_) == 0 && api.getLastError() != WindowsSystem::kErrorSuccess) {
                    return std::unexpected(ErrorCode::ClipboardNativeFailure);
                }
                data_ = nullptr;
                handle_ = nullptr;
                return {};
            }

        private:
            struct LockedData {
                void* value;
            };

            GlobalMemoryLock(WindowsSystem::GlobalMemory handle, LockedData data) noexcept
                : handle_{handle}, data_{data.value} {}

            void UnlockIgnoringError() noexcept {
                if (data_ != nullptr) {
                    data_ = nullptr;
                    const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
                    if (api.globalUnlock != nullptr) {
                        api.globalUnlock(handle_);
                    }
                    handle_ = nullptr;
                }
            }

            WindowsSystem::GlobalMemory handle_ = nullptr;
            void* data_ = nullptr;
        };

        /** @brief Return the bounded byte size of a clipboard global-memory object. */
        [[nodiscard]] Result<WindowsSystem::Size> GlobalMemorySize(WindowsSystem::GlobalMemory handle) {
            const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
            if (api.setLastError == nullptr || api.getLastError == nullptr || api.globalSize == nullptr) {
                return std::unexpected(ErrorCode::NotSupported);
            }
            api.setLastError(WindowsSystem::kErrorSuccess);
            const WindowsSystem::Size size = api.globalSize(handle);
            if (size != 0) {
                return size;
            }
            if (api.getLastError() != WindowsSystem::kErrorSuccess) {
                return std::unexpected(ErrorCode::ClipboardNativeFailure);
            }
            return std::unexpected(ErrorCode::InvalidClipboardData);
        }
#endif

    } // namespace

    Result<std::optional<std::string>> ReadText() {
#ifdef PLATFORM_WINDOWS
        const ClipboardSystemAPI& api = LoadClipboardSystemAPI();
        if (api.isClipboardFormatAvailable == nullptr || api.getClipboardData == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);

        if (api.isClipboardFormatAvailable(WindowsSystem::kUnicodeTextClipboardFormat) == 0) {
            if (auto closed = session.Close(); !closed) {
                return std::unexpected(closed.error());
            }
            return std::optional<std::string>{};
        }

        WindowsSystem::GlobalMemory handle = api.getClipboardData(WindowsSystem::kUnicodeTextClipboardFormat);
        if (handle == nullptr) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        auto byteSize = GlobalMemorySize(handle);
        if (!byteSize) {
            return std::unexpected(byteSize.error());
        }
        if (*byteSize % sizeof(wchar_t) != 0) {
            return std::unexpected(ErrorCode::InvalidClipboardData);
        }

        auto locked = GlobalMemoryLock::Lock(handle);
        if (!locked) {
            return std::unexpected(locked.error());
        }
        const auto* begin = static_cast<const wchar_t*>(locked->Data());
        const size_t capacity = *byteSize / sizeof(wchar_t);
        const auto* terminator = std::find(begin, begin + capacity, L'\0');
        if (terminator == begin + capacity) {
            return std::unexpected(ErrorCode::InvalidClipboardData);
        }
        auto text = Unicode::WideToUtf8(std::wstring_view{begin, static_cast<size_t>(terminator - begin)});
        if (!text) {
            return std::unexpected(text.error());
        }
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
        if (api.globalAllocate == nullptr || api.globalFree == nullptr || api.emptyClipboard == nullptr ||
            api.setClipboardData == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        auto wide = Unicode::Utf8ToWide(text);
        if (!wide) {
            return std::unexpected(wide.error());
        }
        if (wide->size() == std::numeric_limits<size_t>::max() ||
            wide->size() + 1 > std::numeric_limits<WindowsSystem::Size>::max() / sizeof(wchar_t)) {
            return std::unexpected(ErrorCode::ClipboardTextTooLarge);
        }

        const WindowsSystem::Size byteSize = (wide->size() + 1) * sizeof(wchar_t);
        WindowsSystem::GlobalMemory rawMemory = api.globalAllocate(WindowsSystem::kMovableGlobalMemory, byteSize);
        if (rawMemory == nullptr) {
            return std::unexpected(ErrorCode::OutOfMemory);
        }
        OwnedGlobalMemory memory{rawMemory};
        auto locked = GlobalMemoryLock::Lock(memory.Get());
        if (!locked) {
            return std::unexpected(locked.error());
        }
        std::memcpy(locked->Data(), wide->data(), wide->size() * sizeof(wchar_t));
        static_cast<wchar_t*>(locked->Data())[wide->size()] = L'\0';
        if (auto unlocked = locked->Unlock(); !unlocked) {
            return std::unexpected(unlocked.error());
        }

        auto opened = ClipboardSession::Open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        ClipboardSession session = std::move(*opened);
        if (api.emptyClipboard() == 0) {
            return std::unexpected(ErrorCode::ClipboardNativeFailure);
        }
        if (api.setClipboardData(WindowsSystem::kUnicodeTextClipboardFormat, memory.Get()) == nullptr) {
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
