/**
 * @file ResourceStore.h
 * @brief Priority-merged resource lookup over mounted `.lpak` packages.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Resources/PakView.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Sora::Resources {

    /** @brief VFS-like resource store for `.lpak` packages. */
    class ResourceStore {
        struct Mount {
            std::string Name;
            int Priority = 0;
            std::vector<std::byte> Owned;
            std::span<const std::byte> Borrowed{};
            PakView View{};
        };

        struct Resolved {
            uint64_t Hash = 0;
            int Priority = 0;
            size_t MountIndex = 0;
            size_t EntryIndex = 0;
        };

        std::vector<Mount> mounts_;
        std::vector<Resolved> index_;

        [[nodiscard]] auto ContainsMountName(std::string_view name) const noexcept -> bool {
            return std::ranges::any_of(mounts_, [name](const Mount& mount) { return mount.Name == name; });
        }

        void RebuildIndex() {
            index_.clear();
            for (size_t mountIndex = 0; mountIndex < mounts_.size(); ++mountIndex) {
                const auto& mount = mounts_[mountIndex];
                auto entries = mount.View.Entries();
                for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
                    index_.push_back(Resolved{
                        .Hash = entries[entryIndex].semanticHash,
                        .Priority = mount.Priority,
                        .MountIndex = mountIndex,
                        .EntryIndex = entryIndex,
                    });
                }
            }
            std::sort(index_.begin(), index_.end(), [](const Resolved& a, const Resolved& b) {
                if (a.Hash != b.Hash) {
                    return a.Hash < b.Hash;
                }
                if (a.Priority != b.Priority) {
                    return a.Priority > b.Priority;
                }
                return a.MountIndex > b.MountIndex;
            });
            auto out = index_.begin();
            for (auto it = index_.begin(); it != index_.end(); ++it) {
                if (out == index_.begin() || std::prev(out)->Hash != it->Hash) {
                    *out++ = *it;
                }
            }
            index_.erase(out, index_.end());
        }

    public:
        /** @brief Mount an owned `.lpak` byte vector. */
        [[nodiscard]] auto MountPak(std::string name, std::vector<std::byte> bytes, int priority = 0) -> VoidResult {
            if (name.empty() || ContainsMountName(name)) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            Mount mount{
                .Name = std::move(name),
                .Priority = priority,
                .Owned = std::move(bytes),
            };
            auto view = PakView::Open(mount.Owned);
            if (!view) {
                return std::unexpected(view.error());
            }
            mount.View = std::move(*view);
            mounts_.push_back(std::move(mount));
            RebuildIndex();
            return {};
        }

        /** @brief Mount a borrowed `.lpak` byte span, typically from `#embed`. */
        [[nodiscard]] auto MountEmbeddedPak(std::string name, std::span<const std::byte> bytes, int priority = 0)
            -> VoidResult {
            if (name.empty() || ContainsMountName(name)) {
                return std::unexpected(ErrorCode::InvalidArgument);
            }
            auto view = PakView::Open(bytes);
            if (!view) {
                return std::unexpected(view.error());
            }
            mounts_.push_back(Mount{
                .Name = std::move(name),
                .Priority = priority,
                .Borrowed = bytes,
                .View = std::move(*view),
            });
            RebuildIndex();
            return {};
        }

        /** @brief Lookup resource payload by semantic hash. */
        [[nodiscard]] auto Get(uint64_t hash) const -> Result<std::span<const std::byte>> {
            auto it = std::lower_bound(index_.begin(), index_.end(), hash,
                                       [](const Resolved& r, uint64_t h) { return r.Hash < h; });
            if (it == index_.end() || it->Hash != hash) {
                return std::unexpected(ErrorCode::ResourceNotFound);
            }
            const auto& mount = mounts_[it->MountIndex];
            const auto& entry = mount.View.Entries()[it->EntryIndex];
            return mount.View.DataOf(entry);
        }

        /** @brief Number of mounted packages. */
        [[nodiscard]] auto MountCount() const noexcept -> size_t { return mounts_.size(); }

        /** @brief Number of visible resources after priority resolution. */
        [[nodiscard]] auto ResourceCount() const noexcept -> size_t { return index_.size(); }
    };

} // namespace Sora::Resources
