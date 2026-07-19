/**
 * @file Environment.cpp
 * @brief Implement native process-environment access and immutable snapshots.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/Environment.h>
#include <Sora/Core/PAL/SystemAPI.h>
#include <Sora/Core/Unicode.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <mutex>
#include <utility>

#if !defined(PLATFORM_WINDOWS)
extern char** environ;
#endif

namespace Sora::PAL {

    namespace {

        std::mutex gEnvironmentMutex;

        [[nodiscard]] constexpr bool HasEmbeddedNull(std::string_view text) noexcept {
            return text.find('\0') != std::string_view::npos;
        }

        [[nodiscard]] constexpr std::string_view StoredView(const std::string& storage, uint32_t offset,
                                                            uint32_t size) noexcept {
            return {storage.data() + offset, size};
        }

        [[nodiscard]] Result<void> ValidateName(std::string_view name) {
            if (name.empty() || name.contains('=') || HasEmbeddedNull(name) || !Unicode::ValidateUtf8(name)) {
                return std::unexpected(ErrorCode::InvalidEnvironmentName);
            }
            return {};
        }

        [[nodiscard]] Result<void> ValidateValue(std::string_view value) {
            if (HasEmbeddedNull(value) || !Unicode::ValidateUtf8(value)) {
                return std::unexpected(ErrorCode::InvalidEnvironmentValue);
            }
            return {};
        }

#ifdef PLATFORM_WINDOWS
        [[nodiscard]] Result<std::wstring> ToNative(std::string_view text, ErrorCode errorCode) {
            auto wide = Unicode::Utf8ToWide(text);
            if (!wide) {
                return std::unexpected(errorCode);
            }
            return std::move(*wide);
        }
#endif

        [[nodiscard]] Result<std::optional<std::string>> GetUnlocked(std::string_view name) {
#ifdef PLATFORM_WINDOWS
            const EnvironmentSystemAPI& api = LoadEnvironmentSystemAPI();
            if (api.setLastError == nullptr || api.getLastError == nullptr ||
                api.getEnvironmentVariableWide == nullptr) {
                return std::unexpected(ErrorCode::NotSupported);
            }
            auto wideName = ToNative(name, ErrorCode::InvalidEnvironmentName);
            if (!wideName) {
                return std::unexpected(wideName.error());
            }
            for (int attempt = 0; attempt < 4; ++attempt) {
                api.setLastError(WindowsSystem::kErrorSuccess);
                const WindowsSystem::DWord required = api.getEnvironmentVariableWide(wideName->c_str(), nullptr, 0);
                if (required == 0) {
                    const WindowsSystem::DWord native = api.getLastError();
                    if (native == WindowsSystem::kErrorEnvironmentVariableNotFound) {
                        return std::optional<std::string>{};
                    }
                    if (native == WindowsSystem::kErrorSuccess) {
                        return std::optional<std::string>{std::string{}};
                    }
                    return std::unexpected(ErrorCode::EnvironmentNativeFailure);
                }

                std::wstring value(required, L'\0');
                const WindowsSystem::DWord size =
                    api.getEnvironmentVariableWide(wideName->c_str(), value.data(), required);
                if (size < required) {
                    value.resize(size);
                    auto utf8 = Unicode::WideToUtf8(std::wstring_view{value});
                    if (!utf8) {
                        return std::unexpected(ErrorCode::InvalidNativeEnvironmentText);
                    }
                    return std::optional<std::string>{std::move(*utf8)};
                }
            }
            return std::unexpected(ErrorCode::EnvironmentNativeFailure);
#else
            const EnvironmentSystemAPI& api = LoadEnvironmentSystemAPI();
            if (api.get == nullptr) {
                return std::unexpected(ErrorCode::NotSupported);
            }
            const std::string ownedName{name};
            const char* value = api.get(ownedName.c_str());
            if (value == nullptr) {
                return std::optional<std::string>{};
            }
            std::string result{value};
            if (!Unicode::ValidateUtf8(result)) {
                return std::unexpected(ErrorCode::InvalidNativeEnvironmentText);
            }
            return std::optional<std::string>{std::move(result)};
#endif
        }

        [[nodiscard]] Result<void> SetUnlocked(std::string_view name, std::optional<std::string_view> value) {
#ifdef PLATFORM_WINDOWS
            const EnvironmentSystemAPI& api = LoadEnvironmentSystemAPI();
            if (api.setEnvironmentVariableWide == nullptr) {
                return std::unexpected(ErrorCode::NotSupported);
            }
            auto wideName = ToNative(name, ErrorCode::InvalidEnvironmentName);
            if (!wideName) {
                return std::unexpected(wideName.error());
            }
            std::wstring wideValue;
            const wchar_t* nativeValue = nullptr;
            if (value) {
                auto converted = ToNative(*value, ErrorCode::InvalidEnvironmentValue);
                if (!converted) {
                    return std::unexpected(converted.error());
                }
                wideValue = std::move(*converted);
                nativeValue = wideValue.c_str();
            }
            if (api.setEnvironmentVariableWide(wideName->c_str(), nativeValue) == 0) {
                return std::unexpected(ErrorCode::EnvironmentNativeFailure);
            }
#else
            const EnvironmentSystemAPI& api = LoadEnvironmentSystemAPI();
            const std::string ownedName{name};
            if (value) {
                if (api.set == nullptr) {
                    return std::unexpected(ErrorCode::NotSupported);
                }
                const std::string ownedValue{*value};
                if (api.set(ownedName.c_str(), ownedValue.c_str(), 1) != 0) {
                    return std::unexpected(ErrorCode::EnvironmentNativeFailure);
                }
            } else {
                if (api.remove == nullptr) {
                    return std::unexpected(ErrorCode::NotSupported);
                }
                if (api.remove(ownedName.c_str()) != 0) {
                    return std::unexpected(ErrorCode::EnvironmentNativeFailure);
                }
            }
#endif
            return {};
        }

    } // namespace

    EnvironmentEntryView EnvironmentSnapshot::operator[](size_t index) const noexcept {
        const StoredEntry& entry = entries_[index];
        return EnvironmentEntryView{.name = StoredView(storage_, entry.nameOffset, entry.nameSize),
                                    .value = StoredView(storage_, entry.valueOffset, entry.valueSize)};
    }

    std::optional<std::string_view> EnvironmentSnapshot::Find(std::string_view name) const noexcept {
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), name, [this](const StoredEntry& entry, std::string_view candidate) {
                const std::string_view stored = StoredView(storage_, entry.nameOffset, entry.nameSize);
                return CompareEnvironmentNames(stored, candidate) < 0;
            });
        if (iterator == entries_.end()) {
            return std::nullopt;
        }
        const size_t index = static_cast<size_t>(iterator - entries_.begin());
        const EnvironmentEntryView entry = (*this)[index];
        return CompareEnvironmentNames(entry.name, name) == 0 ? std::optional<std::string_view>{entry.value}
                                                              : std::nullopt;
    }

    EnvironmentIndexRange EnvironmentSnapshot::PrefixRange(std::string_view prefix) const noexcept {
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), prefix, [this](const StoredEntry& entry, std::string_view candidate) {
                const std::string_view stored = StoredView(storage_, entry.nameOffset, entry.nameSize);
                return CompareEnvironmentNames(stored, candidate) < 0;
            });
        const size_t begin = static_cast<size_t>(iterator - entries_.begin());
        size_t end = begin;
        while (end < entries_.size() && EnvironmentNameStartsWith((*this)[end].name, prefix)) {
            ++end;
        }
        return EnvironmentIndexRange{.begin = begin, .end = end};
    }

    Result<std::optional<std::string>> ReadEnvironmentVariable(std::string_view name) {
        if (auto valid = ValidateName(name); !valid) {
            return std::unexpected(valid.error());
        }
        const std::scoped_lock lock{gEnvironmentMutex};
        return GetUnlocked(name);
    }

    Result<bool> HasEnvironmentVariable(std::string_view name) {
        auto value = ReadEnvironmentVariable(name);
        if (!value) {
            return std::unexpected(value.error());
        }
        return value->has_value();
    }

    Result<void> WriteEnvironmentVariable(std::string_view name, std::string_view value) {
        if (auto valid = ValidateName(name); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto valid = ValidateValue(value); !valid) {
            return std::unexpected(valid.error());
        }
        const std::scoped_lock lock{gEnvironmentMutex};
        return SetUnlocked(name, value);
    }

    Result<void> RemoveEnvironmentVariable(std::string_view name) {
        if (auto valid = ValidateName(name); !valid) {
            return std::unexpected(valid.error());
        }
        const std::scoped_lock lock{gEnvironmentMutex};
        return SetUnlocked(name, std::nullopt);
    }

    Result<EnvironmentSnapshot> CaptureEnvironment() {
        const std::scoped_lock lock{gEnvironmentMutex};
        EnvironmentSnapshot snapshot;

        auto append = [&snapshot](std::string_view name, std::string_view value) {
            const auto nameOffset = static_cast<uint32_t>(snapshot.storage_.size());
            snapshot.storage_ += name;
            const auto valueOffset = static_cast<uint32_t>(snapshot.storage_.size());
            snapshot.storage_ += value;
            snapshot.entries_.push_back(
                EnvironmentSnapshot::StoredEntry{.nameOffset = nameOffset,
                                                 .nameSize = static_cast<uint32_t>(name.size()),
                                                 .valueOffset = valueOffset,
                                                 .valueSize = static_cast<uint32_t>(value.size())});
        };

#ifdef PLATFORM_WINDOWS
        const EnvironmentSystemAPI& api = LoadEnvironmentSystemAPI();
        if (api.getEnvironmentStringsWide == nullptr || api.freeEnvironmentStringsWide == nullptr) {
            return std::unexpected(ErrorCode::NotSupported);
        }
        wchar_t* block = api.getEnvironmentStringsWide();
        if (block == nullptr) {
            return std::unexpected(ErrorCode::EnvironmentNativeFailure);
        }
        struct BlockGuard {
            wchar_t* value;
            EnvironmentSystemAPI::FreeEnvironmentStringsWideFunction free;
            ~BlockGuard() { free(value); }
        } guard{block, api.freeEnvironmentStringsWide};

        for (const wchar_t* current = block; *current != L'\0';) {
            const std::wstring_view line{current};
            current += line.size() + 1;
            if (line.starts_with(L'=')) {
                continue;
            }
            const size_t separator = line.find(L'=');
            if (separator == std::wstring_view::npos) {
                continue;
            }
            auto name = Unicode::WideToUtf8(line.substr(0, separator));
            auto value = Unicode::WideToUtf8(line.substr(separator + 1));
            if (!name || !value) {
                return std::unexpected(ErrorCode::InvalidNativeEnvironmentText);
            }
            append(*name, *value);
        }
#else
        for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
            const std::string_view line{*current};
            const size_t separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0) {
                continue;
            }
            const std::string_view name = line.substr(0, separator);
            const std::string_view value = line.substr(separator + 1);
            if (!Unicode::ValidateUtf8(name) || !Unicode::ValidateUtf8(value)) {
                return std::unexpected(ErrorCode::InvalidNativeEnvironmentText);
            }
            append(name, value);
        }
#endif

        std::ranges::sort(snapshot.entries_, [&snapshot](const EnvironmentSnapshot::StoredEntry& lhs,
                                                         const EnvironmentSnapshot::StoredEntry& rhs) {
            return CompareEnvironmentNames(StoredView(snapshot.storage_, lhs.nameOffset, lhs.nameSize),
                                           StoredView(snapshot.storage_, rhs.nameOffset, rhs.nameSize)) < 0;
        });
        return snapshot;
    }

    Result<void> ApplyEnvironmentMutations(std::span<const EnvironmentMutation> mutations) {
        for (size_t index = 0; index < mutations.size(); ++index) {
            if (auto valid = ValidateName(mutations[index].name); !valid) {
                return std::unexpected(valid.error());
            }
            if (auto valid = ValidateValue(mutations[index].value.value_or(std::string_view{})); !valid) {
                return std::unexpected(valid.error());
            }
            for (size_t previous = 0; previous < index; ++previous) {
                if (CompareEnvironmentNames(mutations[previous].name, mutations[index].name) == 0) {
                    return std::unexpected(ErrorCode::DuplicateEnvironmentMutation);
                }
            }
        }

        const std::scoped_lock lock{gEnvironmentMutex};
        std::vector<std::optional<std::string>> originals;
        originals.reserve(mutations.size());
        for (const EnvironmentMutation& mutation : mutations) {
            auto original = GetUnlocked(mutation.name);
            if (!original) {
                return std::unexpected(original.error());
            }
            originals.push_back(std::move(*original));
        }

        for (size_t index = 0; index < mutations.size(); ++index) {
            if (auto applied = SetUnlocked(mutations[index].name, mutations[index].value); !applied) {
                for (size_t rollback = index; rollback-- > 0;) {
                    const std::optional<std::string_view> previous = originals[rollback].transform(
                        [](const std::string& value) -> std::string_view { return value; });
                    if (auto restored = SetUnlocked(mutations[rollback].name, previous); !restored) {
                        return std::unexpected(ErrorCode::EnvironmentRollbackFailure);
                    }
                }
                return std::unexpected(applied.error());
            }
        }
        return {};
    }

} // namespace Sora::PAL
