/**
 * @file ConcurrentMap.h
 * @brief Flat concurrent map and set specialized by compile-time reader/writer topology and capacity policy.
 * @ingroup Core
 */
#pragma once

#include "Sora/Platform.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Concurrency/MapTopology.h"
#include "Sora/Core/Hash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora {

    /** @brief Outcome of a non-overwriting associative-container insertion. */
    enum class MapInsertResult : std::uint8_t {
        Inserted,
        Exists,
        Full,
        AllocationFailed,
    };

    /** @brief Outcome of an insert-or-assign map operation. */
    enum class MapAssignResult : std::uint8_t {
        Inserted,
        Assigned,
        Full,
        AllocationFailed,
    };

    namespace Detail {

        struct SetMarker {};

        enum class ConcurrentMapField : std::uint8_t {
            Capacity,
            LocalEntries,
            States,
            Keys,
            Values,
            WriterStripes,
        };

        template<ConcurrentMapField Field>
        using EmptyConcurrentMapField = std::integral_constant<ConcurrentMapField, Field>;

        template<class T>
        class RawMapSlot {
        public:
            RawMapSlot() noexcept = default;

            [[nodiscard]] T* StorageAddress() noexcept { return reinterpret_cast<T*>(storage_); }
            [[nodiscard]] T* Get() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
            [[nodiscard]] const T* Get() const noexcept { return std::launder(reinterpret_cast<const T*>(storage_)); }

        private:
            alignas(T) std::byte storage_[sizeof(T)];
        };

        template<class T>
        consteval bool IsLockFreeAtomicObject() {
            if constexpr (std::is_trivially_copyable_v<T>) {
                return std::atomic_ref<T>::is_always_lock_free;
            } else {
                return false;
            }
        }

        template<class T, std::size_t Extent>
        struct AssociativeArrayType {
            using type = std::array<T, Extent>;
        };

        template<class T>
        struct AssociativeArrayType<T, std::dynamic_extent> {
            using type = std::unique_ptr<T[]>;
        };

        template<class T, std::size_t Extent>
        using AssociativeArray = typename AssociativeArrayType<T, Extent>::type;

        template<class Key, class Mapped, bool IsSet>
        struct LocalMapEntry {
            std::uint64_t state{};
            RawMapSlot<Key> key;
            SORA_NO_UNIQUE_ADDRESS
            std::conditional_t<IsSet, EmptyConcurrentMapField<ConcurrentMapField::Values>, RawMapSlot<Mapped>> value;
        };

        inline void AssociativeSpinPause(std::size_t attempts) noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            __builtin_ia32_pause();
#else
            std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
            if ((attempts & 255U) == 255U) {
                std::this_thread::yield();
            }
        }

        struct alignas(Platform::kCacheLineSize) MapWriterStripe {
            std::atomic_flag locked{};

            void Lock() noexcept {
                std::size_t attempts = 0;
                while (locked.test_and_set(std::memory_order_acquire)) {
                    AssociativeSpinPause(attempts++);
                }
            }

            void Unlock() noexcept { locked.clear(std::memory_order_release); }
        };

        class MapWriterGuard {
        public:
            explicit MapWriterGuard(MapWriterStripe* stripe) noexcept : stripe_{stripe} {
                if (stripe_) {
                    stripe_->Lock();
                }
            }

            MapWriterGuard(const MapWriterGuard&) = delete;
            MapWriterGuard& operator=(const MapWriterGuard&) = delete;

            ~MapWriterGuard() {
                if (stripe_) {
                    stripe_->Unlock();
                }
            }

        private:
            MapWriterStripe* stripe_;
        };

        enum class MapSlotStatus : std::uint64_t {
            Empty = 0,
            Tombstone = 1,
            Full = 2,
            Busy = 3,
        };

        /**
         * @brief Shared flat open-addressing engine used by map and set facades.
         *
         * @details Keys never move after publication. A 64-bit state word carries status, a 16-bit fingerprint, and a
         * 46-bit incarnation. Readers validate the word before and after lock-free atomic key/value loads. A single
         * writer topology emits plain publication stores; multiple writers add hash-striped serialization for equal
         * keys and per-slot CAS only where different probe chains meet.
         */
        template<class Key, class Mapped, std::size_t Capacity, Concurrency::MapTopology Topology,
                 Hashing::AnyAlgo HashAlgorithm, class KeyEqual>
        class FlatConcurrentAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static constexpr bool kDynamic = Capacity == std::dynamic_extent;
            static constexpr bool kLocal = Topology.sharing == Concurrency::MapSharing::SingleThread;
            static constexpr bool kMultipleWriters = Topology.writers == Concurrency::MapCardinality::Multiple;
            static constexpr std::size_t kStripeCount = [] {
                if constexpr (!kMultipleWriters) {
                    return std::size_t{0};
                } else {
                    return kDynamic ? std::size_t{64} : std::min<std::size_t>(Capacity, 64);
                }
            }();

            static_assert(kDynamic || (Capacity > 0 && std::has_single_bit(Capacity)),
                          "flat concurrent associative capacity must be a non-zero power of two");
            static_assert(Concurrency::IsMapTopologyValid(Topology), "invalid map reader/writer topology");
            static_assert(std::is_object_v<Key> && std::is_nothrow_destructible_v<Key> &&
                              std::is_nothrow_copy_constructible_v<Key> && std::is_nothrow_copy_assignable_v<Key>,
                          "ConcurrentMap keys require nothrow copy lifetime operations");
            static_assert(kSet || (std::is_object_v<Mapped> && std::is_nothrow_destructible_v<Mapped> &&
                                   std::is_nothrow_copy_constructible_v<Mapped> &&
                                   std::is_nothrow_copy_assignable_v<Mapped>),
                          "ConcurrentMap values require nothrow copy lifetime operations");
            static_assert(kLocal || IsLockFreeAtomicObject<Key>(),
                          "concurrent topologies require a trivially-copyable lock-free atomic key");
            static_assert(kLocal || kSet || IsLockFreeAtomicObject<Mapped>(),
                          "concurrent map topologies require a trivially-copyable lock-free atomic value");

            using StateWord = std::uint64_t;
            using Size = std::conditional_t<kLocal, std::size_t, std::atomic<std::size_t>>;
            using LocalEntries =
                std::conditional_t<kLocal, AssociativeArray<LocalMapEntry<Key, Mapped, kSet>, Capacity>,
                                   EmptyConcurrentMapField<ConcurrentMapField::LocalEntries>>;
            using States = std::conditional_t<kLocal, EmptyConcurrentMapField<ConcurrentMapField::States>,
                                              AssociativeArray<std::atomic<StateWord>, Capacity>>;
            using KeySlots = std::conditional_t<kLocal, EmptyConcurrentMapField<ConcurrentMapField::Keys>,
                                                AssociativeArray<RawMapSlot<Key>, Capacity>>;
            using ValueSlots = std::conditional_t<kLocal || kSet, EmptyConcurrentMapField<ConcurrentMapField::Values>,
                                                  AssociativeArray<RawMapSlot<Mapped>, Capacity>>;
            using DynamicCapacity =
                std::conditional_t<kDynamic, std::size_t, EmptyConcurrentMapField<ConcurrentMapField::Capacity>>;
            using WriterStripes = std::conditional_t<kMultipleWriters, std::array<MapWriterStripe, kStripeCount>,
                                                     EmptyConcurrentMapField<ConcurrentMapField::WriterStripes>>;

            static_assert(kLocal || std::atomic<StateWord>::is_always_lock_free,
                          "concurrent map topologies require a lock-free slot-state atomic");
            static_assert(kLocal || std::atomic<std::size_t>::is_always_lock_free,
                          "concurrent map topologies require a lock-free size atomic");

            static constexpr unsigned kStatusBits = 2;
            static constexpr unsigned kFingerprintBits = 16;
            static constexpr unsigned kGenerationShift = kStatusBits + kFingerprintBits;
            static constexpr StateWord kStatusMask = (StateWord{1} << kStatusBits) - 1;
            static constexpr StateWord kFingerprintMask = (StateWord{1} << kFingerprintBits) - 1;

        public:
            using key_type = Key;
            using mapped_type = Mapped;
            static constexpr std::size_t staticCapacity = Capacity;
            static constexpr Concurrency::MapTopology topology = Topology;
            static constexpr Concurrency::MapOrder order = Concurrency::MapOrder::Unordered;
            static constexpr bool isSet = kSet;

            FlatConcurrentAssociativeStorage() noexcept
                requires(!kDynamic)
            = default;

            explicit FlatConcurrentAssociativeStorage(std::size_t requestedCapacity) noexcept
                requires kDynamic
                : dynamicCapacity_{NormalizeDynamicCapacity(requestedCapacity)} {
                if constexpr (kLocal) {
                    localEntries_.reset(new (std::nothrow) LocalMapEntry<Key, Mapped, kSet>[dynamicCapacity_]);
                } else {
                    states_.reset(new (std::nothrow) std::atomic<StateWord>[dynamicCapacity_] {});
                    keys_.reset(new (std::nothrow) RawMapSlot<Key>[dynamicCapacity_]);
                    if constexpr (!kSet) {
                        values_.reset(new (std::nothrow) RawMapSlot<Mapped>[dynamicCapacity_]);
                    }
                }
            }
            FlatConcurrentAssociativeStorage(const FlatConcurrentAssociativeStorage&) = delete;
            FlatConcurrentAssociativeStorage& operator=(const FlatConcurrentAssociativeStorage&) = delete;
            FlatConcurrentAssociativeStorage(FlatConcurrentAssociativeStorage&&) = delete;
            FlatConcurrentAssociativeStorage& operator=(FlatConcurrentAssociativeStorage&&) = delete;

            ~FlatConcurrentAssociativeStorage() noexcept { DestroyRetainedObjects(); }

            [[nodiscard]] bool Valid() const noexcept {
                if constexpr (!kDynamic) {
                    return true;
                } else if constexpr (kLocal) {
                    return localEntries_ != nullptr;
                } else if constexpr (kSet) {
                    return states_ && keys_;
                } else {
                    return states_ && keys_ && values_;
                }
            }

            [[nodiscard]] std::size_t CapacityLimit() const noexcept {
                if constexpr (kDynamic) {
                    return dynamicCapacity_;
                } else {
                    return Capacity;
                }
            }

            [[nodiscard]] std::uint64_t HashForKey(const Key& key) const noexcept { return HashKey(key); }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return TryInsertWithHash(key, HashKey(key));
            }

            [[nodiscard]] MapInsertResult TryInsertWithHash(const Key& key, std::uint64_t hash) noexcept
                requires kSet
            {
                return InsertImplWithHash(key, nullptr, false, nullptr, hash).first;
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                return TryInsertWithHash(key, value, HashKey(key));
            }

            [[nodiscard]] MapInsertResult TryInsertWithHash(const Key& key, const Mapped& value,
                                                            std::uint64_t hash) noexcept
                requires(!kSet)
            {
                return InsertImplWithHash(key, std::addressof(value), false, nullptr, hash).first;
            }

            [[nodiscard]] std::pair<MapInsertResult, Mapped*> TryInsertAndGet(const Key& key,
                                                                              const Mapped& value) noexcept
                requires(kLocal && !kSet)
            {
                Mapped* storedValue = nullptr;
                const MapInsertResult result =
                    InsertImplWithHash(key, std::addressof(value), false, &storedValue, HashKey(key)).first;
                return {result, storedValue};
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                return InsertOrAssignWithHash(key, value, HashKey(key));
            }

            [[nodiscard]] MapAssignResult InsertOrAssignWithHash(const Key& key, const Mapped& value,
                                                                 std::uint64_t hash) noexcept
                requires(!kSet)
            {
                const auto [insert, assigned] = InsertImplWithHash(key, std::addressof(value), true, nullptr, hash);
                if (assigned) {
                    return MapAssignResult::Assigned;
                }
                if (insert == MapInsertResult::Inserted) {
                    return MapAssignResult::Inserted;
                }
                return MapAssignResult::Full;
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires(!kSet)
            {
                return FindWithHash(key, HashKey(key));
            }

            [[nodiscard, clang::always_inline]] std::optional<Mapped> FindWithHash(const Key& key,
                                                                                   std::uint64_t hash) const noexcept
                requires(!kSet)
            {
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;

                for (;;) {
                    bool restart = false;
                    for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                        const std::size_t index = (home + distance) & indexMask;
                        const StateWord before = WaitUntilStable(index);
                        switch (Status(before)) {
                            case MapSlotStatus::Empty:
                                return std::nullopt;
                            case MapSlotStatus::Tombstone:
                                continue;
                            case MapSlotStatus::Full:
                                if (StateFingerprint(before) != fingerprint) {
                                    continue;
                                }
                                break;
                            case MapSlotStatus::Busy:
                                std::unreachable();
                        }

                        const Key observedKey = LoadKey(index);
                        if (!keyEqual_(observedKey, key)) {
                            continue;
                        }
                        const Mapped observedValue = LoadValue(index);
                        if (LoadState(index, std::memory_order_acquire) == before) {
                            return observedValue;
                        }
                        restart = true;
                        break;
                    }
                    if (!restart) {
                        return std::nullopt;
                    }
                }
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires(!kSet)
            {
                return TryGetWithHash(key, output, HashKey(key));
            }

            [[nodiscard, clang::always_inline]] bool TryGetWithHash(const Key& key, Mapped& output,
                                                                    std::uint64_t hash) const noexcept
                requires(!kSet)
            {
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;

                for (;;) {
                    bool restart = false;
                    for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                        const std::size_t index = (home + distance) & indexMask;
                        const StateWord before = WaitUntilStable(index);
                        if (Status(before) == MapSlotStatus::Empty) {
                            return false;
                        }
                        if (Status(before) != MapSlotStatus::Full || StateFingerprint(before) != fingerprint) {
                            continue;
                        }
                        const Key observedKey = LoadKey(index);
                        if (!keyEqual_(observedKey, key)) {
                            continue;
                        }
                        const Mapped observedValue = LoadValue(index);
                        if (LoadState(index, std::memory_order_acquire) == before) {
                            output = observedValue;
                            return true;
                        }
                        restart = true;
                        break;
                    }
                    if (!restart) {
                        return false;
                    }
                }
            }

            [[nodiscard, clang::always_inline]] Mapped* FindRef(const Key& key) noexcept
                requires(kLocal && !kSet)
            {
                return FindRefWithHash(key, HashKey(key));
            }

            [[nodiscard, clang::always_inline]] Mapped* FindRefWithHash(const Key& key, std::uint64_t hash) noexcept
                requires(kLocal && !kSet)
            {
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;
                for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                    const std::size_t index = (home + distance) & indexMask;
                    const StateWord state = LoadState(index, std::memory_order_relaxed);
                    if (Status(state) == MapSlotStatus::Empty) {
                        return nullptr;
                    }
                    if (Status(state) == MapSlotStatus::Full && StateFingerprint(state) == fingerprint &&
                        keyEqual_(*KeyPointer(index), key)) {
                        return ValuePointer(index);
                    }
                }
                return nullptr;
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept { return ContainsWithHash(key, HashKey(key)); }

            [[nodiscard, clang::always_inline]] bool ContainsWithHash(const Key& key,
                                                                      std::uint64_t hash) const noexcept {
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;

                for (;;) {
                    bool restart = false;
                    for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                        const std::size_t index = (home + distance) & indexMask;
                        const StateWord before = WaitUntilStable(index);
                        if (Status(before) == MapSlotStatus::Empty) {
                            return false;
                        }
                        if (Status(before) != MapSlotStatus::Full || StateFingerprint(before) != fingerprint) {
                            continue;
                        }
                        const Key observedKey = LoadKey(index);
                        if (LoadState(index, std::memory_order_acquire) != before) {
                            restart = true;
                            break;
                        }
                        if (keyEqual_(observedKey, key)) {
                            return true;
                        }
                    }
                    if (!restart) {
                        return false;
                    }
                }
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept { return EraseWithHash(key, HashKey(key)); }

            [[nodiscard]] bool EraseWithHash(const Key& key, std::uint64_t hash) noexcept {
                return EraseImplWithHash<false>(key, hash, nullptr);
            }

            [[nodiscard]] bool EraseAndGet(const Key& key, Mapped& erasedValue) noexcept
                requires(kLocal && !kSet)
            {
                return EraseImplWithHash<true>(key, HashKey(key), std::addressof(erasedValue));
            }

        private:
            template<bool ReturnErasedValue>
            [[nodiscard]] bool EraseImplWithHash(const Key& key, std::uint64_t hash, Mapped* erasedValue) noexcept {
                MapWriterGuard writer{SelectWriterStripe(hash)};
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;

                for (;;) {
                    bool retry = false;
                    for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                        const std::size_t index = (home + distance) & indexMask;
                        const StateWord state = WaitUntilStable(index);
                        if (Status(state) == MapSlotStatus::Empty) {
                            return false;
                        }
                        if (Status(state) != MapSlotStatus::Full || StateFingerprint(state) != fingerprint ||
                            !keyEqual_(LoadKey(index), key)) {
                            continue;
                        }
                        if (!Claim(index, state, fingerprint)) {
                            retry = true;
                            break;
                        }
                        if constexpr (ReturnErasedValue) {
                            static_assert(!kSet);
                            *erasedValue = LoadValue(index);
                        }
                        Publish(index, state, MapSlotStatus::Tombstone, 0);
                        DecrementSize();
                        return true;
                    }
                    if (!retry) {
                        return false;
                    }
                }
            }

        public:
            /** @brief Approximate concurrent size; exact when no mutation overlaps the call. */
            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                if constexpr (kLocal) {
                    return size_;
                } else {
                    return size_.load(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            /**
             * @brief Copy one writer-quiescent table into an empty table without an intermediate snapshot allocation.
             */
            [[nodiscard]] bool CopyPublishedTo(FlatConcurrentAssociativeStorage& destination) const noexcept {
                for (std::size_t index = 0; index != CapacityLimit(); ++index) {
                    for (;;) {
                        const StateWord before = WaitUntilStable(index);
                        if (Status(before) != MapSlotStatus::Full) {
                            break;
                        }
                        const Key key = LoadKey(index);
                        if constexpr (kSet) {
                            if (LoadState(index, std::memory_order_acquire) != before) {
                                continue;
                            }
                            if (destination.TryInsert(key) != MapInsertResult::Inserted) {
                                return false;
                            }
                        } else {
                            const Mapped value = LoadValue(index);
                            if (LoadState(index, std::memory_order_acquire) != before) {
                                continue;
                            }
                            if (destination.TryInsert(key, value) != MapInsertResult::Inserted) {
                                return false;
                            }
                        }
                        break;
                    }
                }
                return true;
            }

            /**
             * @brief Copy currently published map entries in physical slot order.
             * @details Snapshot allocation is intentionally outside the hot path.
             */
            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires(!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                result.reserve(SizeApprox());
                for (std::size_t index = 0; index != CapacityLimit(); ++index) {
                    for (;;) {
                        const StateWord before = WaitUntilStable(index);
                        if (Status(before) != MapSlotStatus::Full) {
                            break;
                        }
                        Key key = LoadKey(index);
                        Mapped value = LoadValue(index);
                        if (LoadState(index, std::memory_order_acquire) == before) {
                            result.emplace_back(std::move(key), std::move(value));
                            break;
                        }
                    }
                }
                return result;
            }

            /** @brief Copy currently published set keys in physical slot order. */
            [[nodiscard]] std::vector<Key> Snapshot() const
                requires kSet
            {
                std::vector<Key> result;
                result.reserve(SizeApprox());
                for (std::size_t index = 0; index != CapacityLimit(); ++index) {
                    for (;;) {
                        const StateWord before = WaitUntilStable(index);
                        if (Status(before) != MapSlotStatus::Full) {
                            break;
                        }
                        Key key = LoadKey(index);
                        if (LoadState(index, std::memory_order_acquire) == before) {
                            result.emplace_back(std::move(key));
                            break;
                        }
                    }
                }
                return result;
            }

            /** @brief Destroy all retained keys and values. No operation may overlap this quiescent call. */
            void Clear() noexcept {
                if constexpr (kDynamic) {
                    if (!Valid()) {
                        return;
                    }
                }
                for (std::size_t index = 0; index != CapacityLimit(); ++index) {
                    const StateWord state = LoadState(index, std::memory_order_relaxed);
                    if (Status(state) == MapSlotStatus::Full || Status(state) == MapSlotStatus::Tombstone) {
                        DestroyEntry(index);
                    }
                    StoreState(index, 0, std::memory_order_relaxed);
                }
                if constexpr (kLocal) {
                    size_ = 0;
                } else {
                    size_.store(0, std::memory_order_relaxed);
                }
            }

        private:
            [[nodiscard]] static std::size_t NormalizeDynamicCapacity(std::size_t capacity) noexcept {
                constexpr std::size_t kMinimum = 2;
                if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                    return std::bit_floor(std::numeric_limits<std::size_t>::max());
                }
                return std::bit_ceil(std::max(capacity, kMinimum));
            }

            static constexpr bool kRequiresEntryDestruction =
                !std::is_trivially_destructible_v<Key> || (!kSet && !std::is_trivially_destructible_v<Mapped>);

            void DestroyEntry(std::size_t index) noexcept {
                if constexpr (!std::is_trivially_destructible_v<Key>) {
                    std::destroy_at(KeyPointer(index));
                }
                if constexpr (!kSet && !std::is_trivially_destructible_v<Mapped>) {
                    std::destroy_at(ValuePointer(index));
                }
            }

            void DestroyRetainedObjects() noexcept {
                if constexpr (!kRequiresEntryDestruction) {
                    return;
                }
                if constexpr (kDynamic) {
                    if (!Valid()) {
                        return;
                    }
                }
                for (std::size_t index = 0; index != CapacityLimit(); ++index) {
                    const MapSlotStatus status = Status(LoadState(index, std::memory_order_relaxed));
                    if (status == MapSlotStatus::Full || status == MapSlotStatus::Tombstone) {
                        DestroyEntry(index);
                    }
                }
            }

            [[nodiscard]] static constexpr MapSlotStatus Status(StateWord state) noexcept {
                return static_cast<MapSlotStatus>(state & kStatusMask);
            }

            [[nodiscard]] static constexpr StateWord StateFingerprint(StateWord state) noexcept {
                return (state >> kStatusBits) & kFingerprintMask;
            }

            [[nodiscard]] static constexpr StateWord Generation(StateWord state) noexcept {
                return state >> kGenerationShift;
            }

            [[nodiscard]] static constexpr StateWord Encode(StateWord generation, MapSlotStatus status,
                                                            StateWord fingerprint) noexcept {
                return (generation << kGenerationShift) | ((fingerprint & kFingerprintMask) << kStatusBits) |
                       static_cast<StateWord>(status);
            }

            [[nodiscard]] static constexpr std::uint64_t MixHash(std::uint64_t value) noexcept {
                value ^= value >> 30;
                value *= 0xbf58476d1ce4e5b9ULL;
                value ^= value >> 27;
                value *= 0x94d049bb133111ebULL;
                value ^= value >> 31;
                return value;
            }

            [[nodiscard]] std::uint64_t HashKey(const Key& key) const noexcept {
                if constexpr (std::same_as<HashAlgorithm, Hashing::DefaultAlgo> &&
                              (std::is_integral_v<Key> || std::is_enum_v<Key>) &&
                              (sizeof(Key) <= sizeof(std::uint64_t))) {
                    return static_cast<std::uint64_t>(key);
                }
                const auto raw = Hashing::Hash(key, hashAlgorithm_);
                if constexpr (sizeof(raw) <= sizeof(std::uint64_t)) {
                    return MixHash(static_cast<std::uint64_t>(raw));
                } else {
                    return MixHash(static_cast<std::uint64_t>(raw) ^ static_cast<std::uint64_t>(raw >> 64));
                }
            }

            [[nodiscard]] static constexpr StateWord Fingerprint(std::uint64_t hash) noexcept {
                return ((hash >> 32) ^ hash) & kFingerprintMask;
            }

            [[nodiscard]] StateWord LoadState(std::size_t index, std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    static_cast<void>(order);
                    return localEntries_[index].state;
                } else {
                    return states_[index].load(order);
                }
            }

            void StoreState(std::size_t index, StateWord state, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    static_cast<void>(order);
                    localEntries_[index].state = state;
                } else {
                    states_[index].store(state, order);
                }
            }

            [[nodiscard]] StateWord WaitUntilStable(std::size_t index) const noexcept {
                std::size_t attempts = 0;
                for (;;) {
                    const StateWord state = LoadState(index, std::memory_order_acquire);
                    if (Status(state) != MapSlotStatus::Busy) {
                        return state;
                    }
                    AssociativeSpinPause(attempts++);
                }
            }

            [[nodiscard]] bool Claim(std::size_t index, StateWord expected, StateWord fingerprint) noexcept {
                if constexpr (kMultipleWriters) {
                    const StateWord busy = Encode(Generation(expected), MapSlotStatus::Busy, fingerprint);
                    return states_[index].compare_exchange_strong(expected, busy, std::memory_order_acq_rel,
                                                                  std::memory_order_acquire);
                } else {
                    // Empty/tombstone slots remain unreadable until Publish; full-slot readers validate state twice.
                    static_cast<void>(index);
                    static_cast<void>(expected);
                    static_cast<void>(fingerprint);
                    return true;
                }
            }

            void Publish(std::size_t index, StateWord previous, MapSlotStatus status, StateWord fingerprint) noexcept {
                StoreState(index, Encode(Generation(previous) + 1, status, fingerprint), std::memory_order_release);
            }

            [[nodiscard]] Key LoadKey(std::size_t index) const noexcept {
                if constexpr (kLocal) {
                    return *localEntries_[index].key.Get();
                } else {
                    return std::atomic_ref<Key>{*const_cast<Key*>(keys_[index].Get())}.load(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] Mapped LoadValue(std::size_t index) const noexcept
                requires(!kSet)
            {
                if constexpr (kLocal) {
                    return *localEntries_[index].value.Get();
                } else {
                    return std::atomic_ref<Mapped>{*const_cast<Mapped*>(values_[index].Get())}.load(
                        std::memory_order_relaxed);
                }
            }

            void StoreKey(std::size_t index, const Key& key) noexcept {
                if constexpr (kLocal) {
                    *localEntries_[index].key.Get() = key;
                } else {
                    std::atomic_ref<Key>{*keys_[index].Get()}.store(key, std::memory_order_relaxed);
                }
            }

            void StoreValue(std::size_t index, const Mapped& value) noexcept
                requires(!kSet)
            {
                if constexpr (kLocal) {
                    *localEntries_[index].value.Get() = value;
                } else {
                    std::atomic_ref<Mapped>{*values_[index].Get()}.store(value, std::memory_order_relaxed);
                }
            }

            [[nodiscard]] MapWriterStripe* SelectWriterStripe(std::uint64_t hash) noexcept {
                if constexpr (kMultipleWriters) {
                    if constexpr (kStripeCount == 1) {
                        return std::addressof(writerStripes_[0]);
                    } else {
                        constexpr unsigned kStripeBits = std::countr_zero(kStripeCount);
                        constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
                        const std::size_t stripe =
                            static_cast<std::size_t>((hash * kGoldenRatio) >> (64U - kStripeBits));
                        return std::addressof(writerStripes_[stripe]);
                    }
                } else {
                    static_cast<void>(hash);
                    return nullptr;
                }
            }

            void IncrementSize() noexcept {
                if constexpr (kLocal) {
                    ++size_;
                } else if constexpr (kMultipleWriters) {
                    size_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    size_.store(size_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                }
            }

            void DecrementSize() noexcept {
                if constexpr (kLocal) {
                    --size_;
                } else if constexpr (kMultipleWriters) {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    size_.store(size_.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
                }
            }

            [[nodiscard]] std::pair<MapInsertResult, bool> InsertImplWithHash(const Key& key, const Mapped* value,
                                                                              bool assign, Mapped** storedValue,
                                                                              std::uint64_t hash) noexcept {
                MapWriterGuard writer{SelectWriterStripe(hash)};
                const StateWord fingerprint = Fingerprint(hash);
                const std::size_t indexMask = CapacityLimit() - 1;
                const std::size_t home = static_cast<std::size_t>(hash) & indexMask;

                for (;;) {
                    std::size_t tombstone = CapacityLimit();
                    StateWord tombstoneState = 0;
                    bool retry = false;
                    for (std::size_t distance = 0; distance != CapacityLimit(); ++distance) {
                        const std::size_t index = (home + distance) & indexMask;
                        const StateWord state = WaitUntilStable(index);
                        if (Status(state) == MapSlotStatus::Full) {
                            if (StateFingerprint(state) == fingerprint && keyEqual_(LoadKey(index), key)) {
                                if (!assign) {
                                    return {MapInsertResult::Exists, false};
                                }
                                if constexpr (!kSet) {
                                    if (!Claim(index, state, fingerprint)) {
                                        break;
                                    }
                                    StoreValue(index, *value);
                                    Publish(index, state, MapSlotStatus::Full, fingerprint);
                                    if (storedValue) {
                                        *storedValue = ValuePointer(index);
                                    }
                                    return {MapInsertResult::Exists, true};
                                }
                                std::unreachable();
                            }
                            continue;
                        }
                        if (Status(state) == MapSlotStatus::Tombstone) {
                            if (tombstone == CapacityLimit()) {
                                tombstone = index;
                                tombstoneState = state;
                            }
                            continue;
                        }
                        if (Status(state) == MapSlotStatus::Empty) {
                            const std::size_t target = tombstone == CapacityLimit() ? index : tombstone;
                            const StateWord targetState = tombstone == CapacityLimit() ? state : tombstoneState;
                            if (!InsertAt(target, targetState, fingerprint, key, value)) {
                                retry = true;
                                break;
                            }
                            if constexpr (!kSet) {
                                if (storedValue) {
                                    *storedValue = ValuePointer(target);
                                }
                            }
                            return {MapInsertResult::Inserted, false};
                        }
                    }

                    if (retry) {
                        continue;
                    }

                    if (tombstone != CapacityLimit()) {
                        if (InsertAt(tombstone, tombstoneState, fingerprint, key, value)) {
                            if constexpr (!kSet) {
                                if (storedValue) {
                                    *storedValue = ValuePointer(tombstone);
                                }
                            }
                            return {MapInsertResult::Inserted, false};
                        }
                        continue;
                    }
                    return {MapInsertResult::Full, false};
                }
            }

            [[nodiscard]] bool InsertAt(std::size_t index, StateWord state, StateWord fingerprint, const Key& key,
                                        const Mapped* value) noexcept {
                if (!Claim(index, state, fingerprint)) {
                    return false;
                }
                if (Status(state) == MapSlotStatus::Empty) {
                    std::construct_at(KeyStorageAddress(index), key);
                    if constexpr (!kSet) {
                        std::construct_at(ValueStorageAddress(index), *value);
                    }
                } else {
                    StoreKey(index, key);
                    if constexpr (!kSet) {
                        StoreValue(index, *value);
                    }
                }
                Publish(index, state, MapSlotStatus::Full, fingerprint);
                IncrementSize();
                return true;
            }

            [[nodiscard]] Key* KeyPointer(std::size_t index) noexcept {
                if constexpr (kLocal) {
                    return localEntries_[index].key.Get();
                } else {
                    return keys_[index].Get();
                }
            }

            [[nodiscard]] Key* KeyStorageAddress(std::size_t index) noexcept {
                if constexpr (kLocal) {
                    return localEntries_[index].key.StorageAddress();
                } else {
                    return keys_[index].StorageAddress();
                }
            }

            [[nodiscard]] const Key* KeyPointer(std::size_t index) const noexcept {
                if constexpr (kLocal) {
                    return localEntries_[index].key.Get();
                } else {
                    return keys_[index].Get();
                }
            }

            [[nodiscard]] Mapped* ValuePointer(std::size_t index) noexcept
                requires(!kSet)
            {
                if constexpr (kLocal) {
                    return localEntries_[index].value.Get();
                } else {
                    return values_[index].Get();
                }
            }

            [[nodiscard]] Mapped* ValueStorageAddress(std::size_t index) noexcept
                requires(!kSet)
            {
                if constexpr (kLocal) {
                    return localEntries_[index].value.StorageAddress();
                } else {
                    return values_[index].StorageAddress();
                }
            }

            SORA_NO_UNIQUE_ADDRESS DynamicCapacity dynamicCapacity_{};
            SORA_NO_UNIQUE_ADDRESS LocalEntries localEntries_{};
            alignas(kLocal ? 1 : Platform::kCacheLineSize) SORA_NO_UNIQUE_ADDRESS States states_{};
            alignas(kLocal ? 1 : Platform::kCacheLineSize) SORA_NO_UNIQUE_ADDRESS KeySlots keys_{};
            SORA_NO_UNIQUE_ADDRESS ValueSlots values_{};
            alignas(kLocal ? alignof(Size) : Platform::kCacheLineSize) Size size_{};
            SORA_NO_UNIQUE_ADDRESS WriterStripes writerStripes_{};
            SORA_NO_UNIQUE_ADDRESS HashAlgorithm hashAlgorithm_{};
            SORA_NO_UNIQUE_ADDRESS KeyEqual keyEqual_{};
        };

        /**
         * @brief Local flat index over geometrically segmented stable value slots.
         *
         * @details Map values occupy stable slots and the replaceable index owns each key plus one slot pointer, so
         * growth never moves values returned by FindRef. Erased slots form an intrusive free list. Sets need no stable
         * value arena and therefore compile to only the replaceable flat index.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class IndexedDynamicLocalAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static_assert(Topology.sharing == Concurrency::MapSharing::SingleThread);

            struct StableValueSlot {
                union Storage {
                    Storage() noexcept {}
                    ~Storage() noexcept {}

                    RawMapSlot<Mapped> value;
                    StableValueSlot* nextFree;
                };

                [[nodiscard]] Mapped* Value() noexcept { return storage.value.Get(); }
                [[nodiscard]] const Mapped* Value() const noexcept { return storage.value.Get(); }

                void ConstructValue(const Mapped& source) noexcept {
                    std::construct_at(std::addressof(storage.value));
                    std::construct_at(storage.value.StorageAddress(), source);
                    live = true;
                }

                void DestroyValue() noexcept {
                    std::destroy_at(Value());
                    std::destroy_at(std::addressof(storage.value));
                    live = false;
                }

                void LinkFree(StableValueSlot* next) noexcept {
                    std::construct_at(std::addressof(storage.nextFree), next);
                }

                [[nodiscard]] StableValueSlot* UnlinkFree() noexcept {
                    StableValueSlot* next = storage.nextFree;
                    std::destroy_at(std::addressof(storage.nextFree));
                    return next;
                }

                Storage storage;
                bool live;
            };

            using IndexValue = std::conditional_t<kSet, SetMarker, StableValueSlot*>;
            using IndexStorage = FlatConcurrentAssociativeStorage<Key, IndexValue, std::dynamic_extent, Topology,
                                                                  HashAlgorithm, KeyEqual>;

            struct Segment {
                explicit Segment(std::size_t slotCapacity, Segment* older) noexcept
                    : slots{new (std::nothrow) StableValueSlot[slotCapacity]}, capacity{slotCapacity}, next{older} {}

                ~Segment() noexcept { Clear(); }

                [[nodiscard]] bool Valid() const noexcept { return slots != nullptr; }

                void Clear() noexcept {
                    for (std::size_t index = 0; index != used; ++index) {
                        if (slots[index].live) {
                            slots[index].DestroyValue();
                        }
                    }
                    used = 0;
                }

                std::unique_ptr<StableValueSlot[]> slots;
                std::size_t capacity;
                std::size_t used{};
                Segment* next;
            };

        public:
            explicit IndexedDynamicLocalAssociativeStorage(std::size_t initialCapacity = 64) noexcept
                : initialCapacity_{NormalizeCapacity(initialCapacity)},
                  head_{CreateInitialSegment(initialCapacity_)},
                  index_{CreateIndex(initialCapacity_)} {}

            IndexedDynamicLocalAssociativeStorage(const IndexedDynamicLocalAssociativeStorage&) = delete;
            IndexedDynamicLocalAssociativeStorage& operator=(const IndexedDynamicLocalAssociativeStorage&) = delete;
            IndexedDynamicLocalAssociativeStorage(IndexedDynamicLocalAssociativeStorage&&) = delete;
            IndexedDynamicLocalAssociativeStorage& operator=(IndexedDynamicLocalAssociativeStorage&&) = delete;

            ~IndexedDynamicLocalAssociativeStorage() noexcept { DestroySegments(head_); }

            [[nodiscard]] bool Valid() const noexcept {
                return index_ && index_->Valid() && (kSet || head_ != nullptr);
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return InsertSet(key);
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                return InsertMap(key, value);
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                if (!index_) {
                    return MapAssignResult::AllocationFailed;
                }
                const std::uint64_t hash = index_->HashForKey(key);
                if (StableValueSlot** location = index_->FindRefWithHash(key, hash)) {
                    *(*location)->Value() = value;
                    return MapAssignResult::Assigned;
                }
                if (IndexNeedsGrowth() && !GrowIndex()) {
                    return MapAssignResult::AllocationFailed;
                }
                const MapInsertResult result = InsertMapAbsentWithHash(key, value, hash);
                if (result == MapInsertResult::Inserted) {
                    return MapAssignResult::Inserted;
                }
                return result == MapInsertResult::AllocationFailed ? MapAssignResult::AllocationFailed
                                                                   : MapAssignResult::Full;
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires(!kSet)
            {
                if (!index_) {
                    return std::nullopt;
                }
                const auto location = index_->Find(key);
                return location ? std::optional<Mapped>{*(*location)->Value()} : std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires(!kSet)
            {
                if (!index_) {
                    return false;
                }
                const auto location = index_->Find(key);
                if (!location) {
                    return false;
                }
                output = *(*location)->Value();
                return true;
            }

            [[nodiscard, clang::always_inline]] Mapped* FindRef(const Key& key) noexcept
                requires(!kSet)
            {
                if (!index_) {
                    return nullptr;
                }
                StableValueSlot** location = index_->FindRef(key);
                return location ? (*location)->Value() : nullptr;
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept { return index_ && index_->Contains(key); }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                if (!index_) {
                    return false;
                }
                if constexpr (kSet) {
                    if (!index_->Erase(key)) {
                        return false;
                    }
                } else {
                    StableValueSlot* location = nullptr;
                    if (!index_->EraseAndGet(key, location)) {
                        return false;
                    }
                    ReleaseValue(location);
                }
                --size_;
                return true;
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept { return size_; }
            [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires(!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                result.reserve(size_);
                if (!index_) {
                    return result;
                }
                for (auto& [key, slot] : index_->Snapshot()) {
                    result.emplace_back(std::move(key), *slot->Value());
                }
                return result;
            }

            [[nodiscard]] std::vector<Key> Snapshot() const
                requires kSet
            {
                return index_ ? index_->Snapshot() : std::vector<Key>{};
            }

            /** @brief Remove all entries while retaining the oldest value segment and current index allocation. */
            void Clear() noexcept {
                if (index_) {
                    index_->Clear();
                } else {
                    index_ = CreateIndex(initialCapacity_);
                }
                if constexpr (!kSet) {
                    ClearValueSegments();
                }
                size_ = 0;
            }

        private:
            [[nodiscard]] static std::size_t NormalizeCapacity(std::size_t capacity) noexcept {
                constexpr std::size_t kMinimum = 8;
                if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                    return std::bit_floor(std::numeric_limits<std::size_t>::max());
                }
                return std::bit_ceil(std::max(capacity, kMinimum));
            }

            [[nodiscard]] static Segment* CreateSegment(std::size_t capacity, Segment* next) noexcept {
                void* memory = ::operator new(sizeof(Segment), std::align_val_t{alignof(Segment)}, std::nothrow);
                if (!memory) {
                    return nullptr;
                }
                Segment* segment = std::construct_at(static_cast<Segment*>(memory), capacity, next);
                if (!segment->Valid()) {
                    std::destroy_at(segment);
                    ::operator delete(memory, std::align_val_t{alignof(Segment)});
                    return nullptr;
                }
                return segment;
            }

            [[nodiscard]] static Segment* CreateInitialSegment(std::size_t capacity) noexcept {
                if constexpr (kSet) {
                    static_cast<void>(capacity);
                    return nullptr;
                } else {
                    return CreateSegment(capacity, nullptr);
                }
            }

            [[nodiscard]] static std::unique_ptr<IndexStorage> CreateIndex(std::size_t capacity) noexcept {
                std::unique_ptr<IndexStorage> index{new (std::nothrow) IndexStorage(capacity)};
                return index && index->Valid() ? std::move(index) : nullptr;
            }

            static void DestroySegment(Segment* segment) noexcept {
                std::destroy_at(segment);
                ::operator delete(segment, std::align_val_t{alignof(Segment)});
            }

            static void DestroySegments(Segment* segment) noexcept {
                while (segment) {
                    Segment* next = segment->next;
                    DestroySegment(segment);
                    segment = next;
                }
            }

            void ClearValueSegments() noexcept
                requires(!kSet)
            {
                freeValues_ = nullptr;
                if (!head_) {
                    head_ = CreateSegment(initialCapacity_, nullptr);
                    return;
                }
                Segment* oldest = head_;
                while (oldest->next) {
                    oldest = oldest->next;
                }
                Segment* current = head_;
                while (current != oldest) {
                    Segment* next = current->next;
                    DestroySegment(current);
                    current = next;
                }
                oldest->Clear();
                oldest->next = nullptr;
                head_ = oldest;
            }

            [[nodiscard]] bool RebuildIndex(std::size_t capacity) noexcept {
                auto replacement = CreateIndex(capacity);
                if (!replacement || !index_->CopyPublishedTo(*replacement)) {
                    return false;
                }
                index_ = std::move(replacement);
                return true;
            }

            [[nodiscard]] static std::size_t GrownIndexCapacity(std::size_t capacity) noexcept {
                if (capacity <= std::numeric_limits<std::size_t>::max() / 4) {
                    return capacity * 4;
                }
                return capacity <= std::numeric_limits<std::size_t>::max() / 2 ? capacity * 2 : 0;
            }

            [[nodiscard]] bool GrowIndex() noexcept {
                const std::size_t capacity = index_->CapacityLimit();
                const std::size_t grownCapacity = GrownIndexCapacity(capacity);
                return grownCapacity != 0 && RebuildIndex(grownCapacity);
            }

            [[nodiscard]] bool IndexNeedsGrowth() const noexcept {
                const std::size_t capacity = index_->CapacityLimit();
                return size_ >= capacity - capacity / 4;
            }

            [[nodiscard]] MapInsertResult InsertIndexWithHash(const Key& key, const IndexValue& location,
                                                              std::uint64_t hash) noexcept {
                MapInsertResult result;
                if constexpr (kSet) {
                    static_cast<void>(location);
                    result = index_->TryInsertWithHash(key, hash);
                } else {
                    result = index_->TryInsertWithHash(key, location, hash);
                }
                if (result != MapInsertResult::Full) {
                    return result;
                }
                const std::size_t capacity = index_->CapacityLimit();
                const std::size_t grownCapacity = GrownIndexCapacity(capacity);
                if (grownCapacity == 0 || !RebuildIndex(grownCapacity)) {
                    return MapInsertResult::AllocationFailed;
                }
                if constexpr (kSet) {
                    return index_->TryInsertWithHash(key, hash);
                } else {
                    return index_->TryInsertWithHash(key, location, hash);
                }
            }

            [[nodiscard]] StableValueSlot* AllocateValue(const Mapped& value) noexcept
                requires(!kSet)
            {
                StableValueSlot* slot = freeValues_;
                if (slot) {
                    freeValues_ = slot->UnlinkFree();
                } else {
                    if (!head_ || head_->used == head_->capacity) {
                        const std::size_t capacity = head_ ? head_->capacity : initialCapacity_;
                        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                            return nullptr;
                        }
                        Segment* segment = CreateSegment(head_ ? capacity * 2 : capacity, head_);
                        if (!segment) {
                            return nullptr;
                        }
                        head_ = segment;
                    }
                    slot = std::addressof(head_->slots[head_->used++]);
                }
                slot->ConstructValue(value);
                return slot;
            }

            void ReleaseValue(StableValueSlot* slot) noexcept
                requires(!kSet)
            {
                slot->DestroyValue();
                slot->LinkFree(freeValues_);
                freeValues_ = slot;
            }

            [[nodiscard]] MapInsertResult InsertSet(const Key& key) noexcept
                requires kSet
            {
                if (!index_) {
                    return MapInsertResult::AllocationFailed;
                }
                const std::uint64_t hash = index_->HashForKey(key);
                if (IndexNeedsGrowth()) {
                    if (index_->ContainsWithHash(key, hash)) {
                        return MapInsertResult::Exists;
                    }
                    if (!GrowIndex()) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
                const MapInsertResult result = InsertIndexWithHash(key, {}, hash);
                if (result == MapInsertResult::Inserted) {
                    ++size_;
                }
                return result;
            }

            [[nodiscard]] MapInsertResult InsertMap(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                if (!index_) {
                    return MapInsertResult::AllocationFailed;
                }
                const std::uint64_t hash = index_->HashForKey(key);
                if (IndexNeedsGrowth()) {
                    if (index_->ContainsWithHash(key, hash)) {
                        return MapInsertResult::Exists;
                    }
                    if (!GrowIndex()) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
                return InsertMapAbsentWithHash(key, value, hash);
            }

            [[nodiscard]] MapInsertResult InsertMapAbsentWithHash(const Key& key, const Mapped& value,
                                                                  std::uint64_t hash) noexcept
                requires(!kSet)
            {
                StableValueSlot* slot = AllocateValue(value);
                if (!slot) {
                    return MapInsertResult::AllocationFailed;
                }
                const MapInsertResult indexResult = InsertIndexWithHash(key, slot, hash);
                if (indexResult != MapInsertResult::Inserted) {
                    ReleaseValue(slot);
                    return indexResult;
                }
                ++size_;
                return MapInsertResult::Inserted;
            }

            const std::size_t initialCapacity_;
            Segment* head_{};
            StableValueSlot* freeValues_{};
            std::unique_ptr<IndexStorage> index_;
            std::size_t size_{};
        };

        /**
         * @brief Read-optimized dynamic shard using release-published copy-on-grow table versions.
         *
         * @details The shard serializes its writers, so its flat table is compiled as a single-writer data plane even
         * when the public topology permits multiple writers. Single-writer shards use a capacity-tiered growth schedule
         * that minimizes cumulative copying while avoiding excessive final capacity; multiple-writer shards double to
         * control aggregate over-reservation. Readers acquire one version and probe only that table. Retired versions
         * remain alive until quiescent clear or destruction, without reader registration or reclamation traffic on the
         * lookup path. Because every growth factor is at least two, retained historical capacity is less than the
         * current version's capacity; peak residency still follows the largest table reached until quiescent cleanup.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class VersionedConcurrentAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static constexpr bool kMultipleWriters = Topology.writers == Concurrency::MapCardinality::Multiple;
            static constexpr std::size_t kWriterStripeCount = kMultipleWriters ? 1 : 0;
            static constexpr Concurrency::MapTopology kFlatTopology{
                Concurrency::MapCardinality::Single,
                Topology.readers,
                Concurrency::MapSharing::Concurrent,
            };
            using TableStorage = FlatConcurrentAssociativeStorage<Key, Mapped, std::dynamic_extent, kFlatTopology,
                                                                  HashAlgorithm, KeyEqual>;

            struct Version {
                explicit Version(std::size_t capacity, Version* retiredOlder) noexcept
                    : storage{capacity}, older{retiredOlder} {}

                TableStorage storage;
                Version* older;
            };

            using WriterStripes = std::conditional_t<kMultipleWriters, std::array<MapWriterStripe, kWriterStripeCount>,
                                                     EmptyConcurrentMapField<ConcurrentMapField::WriterStripes>>;

        public:
            explicit VersionedConcurrentAssociativeStorage(std::size_t initialCapacity = 64) noexcept
                : initialCapacity_{NormalizeCapacity(initialCapacity)},
                  current_{CreateVersion(initialCapacity_, nullptr)} {}

            VersionedConcurrentAssociativeStorage(const VersionedConcurrentAssociativeStorage&) = delete;
            VersionedConcurrentAssociativeStorage& operator=(const VersionedConcurrentAssociativeStorage&) = delete;
            VersionedConcurrentAssociativeStorage(VersionedConcurrentAssociativeStorage&&) = delete;
            VersionedConcurrentAssociativeStorage& operator=(VersionedConcurrentAssociativeStorage&&) = delete;

            ~VersionedConcurrentAssociativeStorage() noexcept {
                DestroyVersions(current_.load(std::memory_order_relaxed));
            }

            [[nodiscard]] bool Valid() const noexcept { return current_.load(std::memory_order_relaxed) != nullptr; }

            [[nodiscard]] std::uint64_t HashForKey(const Key& key) const noexcept { return HashKey(key); }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return TryInsertWithHash(key, HashKey(key));
            }

            [[nodiscard]] MapInsertResult TryInsertWithHash(const Key& key, std::uint64_t hash) noexcept
                requires kSet
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapInsertResult::AllocationFailed;
                    }
                    const MapInsertResult result = current->storage.TryInsertWithHash(key, hash);
                    if (result == MapInsertResult::Inserted) {
                        if (ShouldGrow(current)) {
                            static_cast<void>(Grow(current));
                        }
                        return result;
                    }
                    if (result != MapInsertResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                return TryInsertWithHash(key, value, HashKey(key));
            }

            [[nodiscard]] MapInsertResult TryInsertWithHash(const Key& key, const Mapped& value,
                                                            std::uint64_t hash) noexcept
                requires(!kSet)
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapInsertResult::AllocationFailed;
                    }
                    const MapInsertResult result = current->storage.TryInsertWithHash(key, value, hash);
                    if (result == MapInsertResult::Inserted) {
                        if (ShouldGrow(current)) {
                            static_cast<void>(Grow(current));
                        }
                        return result;
                    }
                    if (result != MapInsertResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                return InsertOrAssignWithHash(key, value, HashKey(key));
            }

            [[nodiscard]] MapAssignResult InsertOrAssignWithHash(const Key& key, const Mapped& value,
                                                                 std::uint64_t hash) noexcept
                requires(!kSet)
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapAssignResult::AllocationFailed;
                    }
                    const MapAssignResult result = current->storage.InsertOrAssignWithHash(key, value, hash);
                    if (result == MapAssignResult::Inserted) {
                        if (ShouldGrow(current)) {
                            static_cast<void>(Grow(current));
                        }
                        return result;
                    }
                    if (result != MapAssignResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapAssignResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires(!kSet)
            {
                return FindWithHash(key, HashForKey(key));
            }

            [[nodiscard, clang::always_inline]] std::optional<Mapped> FindWithHash(const Key& key,
                                                                                   std::uint64_t hash) const noexcept
                requires(!kSet)
            {
                Version* current = LoadCurrent();
                return current ? current->storage.FindWithHash(key, hash) : std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires(!kSet)
            {
                return TryGetWithHash(key, output, HashForKey(key));
            }

            [[nodiscard, clang::always_inline]] bool TryGetWithHash(const Key& key, Mapped& output,
                                                                    std::uint64_t hash) const noexcept
                requires(!kSet)
            {
                Version* current = LoadCurrent();
                return current && current->storage.TryGetWithHash(key, output, hash);
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                return ContainsWithHash(key, HashForKey(key));
            }

            [[nodiscard, clang::always_inline]] bool ContainsWithHash(const Key& key,
                                                                      std::uint64_t hash) const noexcept {
                Version* current = LoadCurrent();
                return current && current->storage.ContainsWithHash(key, hash);
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept { return EraseWithHash(key, HashForKey(key)); }

            [[nodiscard]] bool EraseWithHash(const Key& key, std::uint64_t hash) noexcept {
                MapWriterGuard writer{SelectWriterStripe()};
                Version* current = LoadCurrent();
                return current && current->storage.EraseWithHash(key, hash);
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                Version* current = LoadCurrent();
                return current ? current->storage.SizeApprox() : 0;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires(!kSet)
            {
                Version* current = LoadCurrent();
                return current ? current->storage.Snapshot() : std::vector<std::pair<Key, Mapped>>{};
            }

            [[nodiscard]] std::vector<Key> Snapshot() const
                requires kSet
            {
                Version* current = LoadCurrent();
                return current ? current->storage.Snapshot() : std::vector<Key>{};
            }

            /** @brief Remove every retained version and restore initial capacity. Requires quiescence. */
            void Clear() noexcept {
                Version* retired = current_.load(std::memory_order_relaxed);
                Version* fresh = CreateVersion(initialCapacity_, nullptr);
                if (fresh) {
                    current_.store(fresh, std::memory_order_relaxed);
                    DestroyVersions(retired);
                    return;
                }
                if (!retired) {
                    return;
                }
                retired->storage.Clear();
                DestroyVersions(std::exchange(retired->older, nullptr));
            }

        private:
            static_assert(Topology.sharing == Concurrency::MapSharing::Concurrent);
            static_assert(std::atomic<Version*>::is_always_lock_free,
                          "dynamic concurrent map versions require a lock-free publication pointer");

            [[nodiscard]] static std::size_t NormalizeCapacity(std::size_t capacity) noexcept {
                constexpr std::size_t kMinimum = 2;
                if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                    return std::bit_floor(std::numeric_limits<std::size_t>::max());
                }
                return std::bit_ceil(std::max(capacity, kMinimum));
            }

            [[nodiscard]] static Version* CreateVersion(std::size_t capacity, Version* older) noexcept {
                void* memory = ::operator new(sizeof(Version), std::align_val_t{alignof(Version)}, std::nothrow);
                if (!memory) {
                    return nullptr;
                }
                Version* version = std::construct_at(static_cast<Version*>(memory), capacity, older);
                if (!version->storage.Valid()) {
                    std::destroy_at(version);
                    ::operator delete(memory, std::align_val_t{alignof(Version)});
                    return nullptr;
                }
                return version;
            }

            static void DestroyVersion(Version* version) noexcept {
                std::destroy_at(version);
                ::operator delete(version, std::align_val_t{alignof(Version)});
            }

            static void DestroyVersions(Version* version) noexcept {
                while (version) {
                    Version* older = version->older;
                    DestroyVersion(version);
                    version = older;
                }
            }

            [[nodiscard]] Version* LoadCurrent() const noexcept { return current_.load(std::memory_order_acquire); }

            [[nodiscard]] MapWriterStripe* SelectWriterStripe() noexcept {
                if constexpr (kMultipleWriters) {
                    return std::addressof(writerStripes_[0]);
                } else {
                    return nullptr;
                }
            }

            [[nodiscard]] static constexpr std::size_t GrowthFactor(std::size_t capacity) noexcept {
                if constexpr (kMultipleWriters) {
                    return 2;
                }
                constexpr std::size_t kSmallCapacityLimit = 4U * 1024U;
                constexpr std::size_t kCompactCapacityLimit = 16U * 1024U;
                constexpr std::size_t kMediumCapacityLimit = 32U * 1024U;
                if (capacity < kSmallCapacityLimit) {
                    return 8;
                }
                if (capacity < kCompactCapacityLimit) {
                    return 4;
                }
                return capacity < kMediumCapacityLimit ? 2 : 4;
            }

            [[nodiscard]] bool Grow(Version* current) noexcept {
                const std::size_t capacity = current->storage.CapacityLimit();
                const std::size_t growthFactor = GrowthFactor(capacity);
                if (capacity > std::numeric_limits<std::size_t>::max() / growthFactor) {
                    return false;
                }
                Version* next = CreateVersion(capacity * growthFactor, current);
                if (!next) {
                    return false;
                }
                if (!current->storage.CopyPublishedTo(next->storage)) {
                    DestroyVersion(next);
                    return false;
                }
                current_.store(next, std::memory_order_release);
                return true;
            }

            [[nodiscard]] static bool ShouldGrow(const Version* current) noexcept {
                const std::size_t capacity = current->storage.CapacityLimit();
                return current->storage.SizeApprox() >= capacity - capacity / 4;
            }

            [[nodiscard]] static constexpr std::uint64_t MixHash(std::uint64_t value) noexcept {
                value ^= value >> 30;
                value *= 0xbf58476d1ce4e5b9ULL;
                value ^= value >> 27;
                value *= 0x94d049bb133111ebULL;
                value ^= value >> 31;
                return value;
            }

            [[nodiscard]] std::uint64_t HashKey(const Key& key) const noexcept {
                if constexpr (std::same_as<HashAlgorithm, Hashing::DefaultAlgo> &&
                              (std::is_integral_v<Key> || std::is_enum_v<Key>) &&
                              (sizeof(Key) <= sizeof(std::uint64_t))) {
                    return static_cast<std::uint64_t>(key);
                }
                const auto raw = Hashing::Hash(key, hashAlgorithm_);
                if constexpr (sizeof(raw) <= sizeof(std::uint64_t)) {
                    return MixHash(static_cast<std::uint64_t>(raw));
                } else {
                    return MixHash(static_cast<std::uint64_t>(raw) ^ static_cast<std::uint64_t>(raw >> 64));
                }
            }

            const std::size_t initialCapacity_;
            alignas(Platform::kCacheLineSize) std::atomic<Version*> current_{};
            SORA_NO_UNIQUE_ADDRESS WriterStripes writerStripes_{};
            SORA_NO_UNIQUE_ADDRESS HashAlgorithm hashAlgorithm_{};
        };

        /**
         * @brief Hash-sharded dynamic storage with one release-published flat table per shard.
         *
         * @details Multiple-writer topologies use a compile-time shard count to partition contention while each shard
         * retains the topology-required writer arbitration. Single-writer topologies compile to one shard and pay no
         * unnecessary partitioning cost. Lookups acquire one version and probe one flat table.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class ShardedDynamicConcurrentAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static constexpr bool kMultipleWriters = Topology.writers == Concurrency::MapCardinality::Multiple;
            static constexpr std::size_t kShardCount = kMultipleWriters ? 16 : 1;
            using ShardStorage = VersionedConcurrentAssociativeStorage<Key, Mapped, Topology, HashAlgorithm, KeyEqual>;

        public:
            explicit ShardedDynamicConcurrentAssociativeStorage(std::size_t initialCapacity = 64) noexcept {
                const std::size_t shardCapacity = ShardInitialCapacity(initialCapacity);
                for (auto& shard : shards_) {
                    ShardStorage* storage = std::construct_at(shard.StorageAddress(), shardCapacity);
                    if (!storage->Valid()) {
                        valid_ = false;
                    }
                }
            }

            ShardedDynamicConcurrentAssociativeStorage(const ShardedDynamicConcurrentAssociativeStorage&) = delete;
            ShardedDynamicConcurrentAssociativeStorage&
            operator=(const ShardedDynamicConcurrentAssociativeStorage&) = delete;
            ShardedDynamicConcurrentAssociativeStorage(ShardedDynamicConcurrentAssociativeStorage&&) = delete;
            ShardedDynamicConcurrentAssociativeStorage&
            operator=(ShardedDynamicConcurrentAssociativeStorage&&) = delete;

            ~ShardedDynamicConcurrentAssociativeStorage() noexcept {
                for (auto& shard : shards_) {
                    std::destroy_at(shard.Get());
                }
            }

            [[nodiscard]] bool Valid() const noexcept { return valid_; }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                if (!valid_) {
                    return MapInsertResult::AllocationFailed;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->TryInsertWithHash(key, hash);
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                if (!valid_) {
                    return MapInsertResult::AllocationFailed;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->TryInsertWithHash(key, value, hash);
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires(!kSet)
            {
                if (!valid_) {
                    return MapAssignResult::AllocationFailed;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->InsertOrAssignWithHash(key, value, hash);
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires(!kSet)
            {
                if (!valid_) {
                    return std::nullopt;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->FindWithHash(key, hash);
            }

            [[nodiscard, clang::always_inline]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires(!kSet)
            {
                if (!valid_) {
                    return false;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->TryGetWithHash(key, output, hash);
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                if (!valid_) {
                    return false;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->ContainsWithHash(key, hash);
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                if (!valid_) {
                    return false;
                }
                const std::uint64_t hash = HashKey(key);
                return shards_[SelectShardIndex(hash)].Get()->EraseWithHash(key, hash);
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                std::size_t size = 0;
                for (const auto& shard : shards_) {
                    size += shard.Get()->SizeApprox();
                }
                return size;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires(!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                result.reserve(SizeApprox());
                for (const auto& shard : shards_) {
                    auto entries = shard.Get()->Snapshot();
                    result.insert(result.end(), std::make_move_iterator(entries.begin()),
                                  std::make_move_iterator(entries.end()));
                }
                return result;
            }

            [[nodiscard]] std::vector<Key> Snapshot() const
                requires kSet
            {
                std::vector<Key> result;
                result.reserve(SizeApprox());
                for (const auto& shard : shards_) {
                    auto keys = shard.Get()->Snapshot();
                    result.insert(result.end(), std::make_move_iterator(keys.begin()),
                                  std::make_move_iterator(keys.end()));
                }
                return result;
            }

            void Clear() noexcept {
                for (auto& shard : shards_) {
                    shard.Get()->Clear();
                }
            }

        private:
            static_assert(Topology.sharing == Concurrency::MapSharing::Concurrent);
            static_assert(std::has_single_bit(kShardCount));

            [[nodiscard]] static constexpr std::size_t ShardInitialCapacity(std::size_t totalCapacity) noexcept {
                return std::max<std::size_t>(
                    totalCapacity / kShardCount + static_cast<std::size_t>(totalCapacity % kShardCount != 0), 2);
            }

            [[nodiscard]] std::uint64_t HashKey(const Key& key) const noexcept {
                return shards_[0].Get()->HashForKey(key);
            }

            [[nodiscard]] static constexpr std::size_t SelectShardIndex(std::uint64_t hash) noexcept {
                if constexpr (kShardCount == 1) {
                    static_cast<void>(hash);
                    return 0;
                } else {
                    constexpr unsigned kShardBits = std::countr_zero(kShardCount);
                    constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
                    return static_cast<std::size_t>((hash * kGoldenRatio) >> (64U - kShardBits));
                }
            }

            std::array<RawMapSlot<ShardStorage>, kShardCount> shards_{};
            bool valid_ = true;
        };

        template<class Key, class Mapped, std::size_t Capacity, Concurrency::MapTopology Topology,
                 Hashing::AnyAlgo HashAlgorithm, class KeyEqual>
        struct AssociativeStorageType {
            using type = FlatConcurrentAssociativeStorage<Key, Mapped, Capacity, Topology, HashAlgorithm, KeyEqual>;
        };

        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        struct AssociativeStorageType<Key, Mapped, std::dynamic_extent, Topology, HashAlgorithm, KeyEqual> {
            using type = std::conditional_t<
                Topology.sharing == Concurrency::MapSharing::SingleThread,
                IndexedDynamicLocalAssociativeStorage<Key, Mapped, Topology, HashAlgorithm, KeyEqual>,
                ShardedDynamicConcurrentAssociativeStorage<Key, Mapped, Topology, HashAlgorithm, KeyEqual>>;
        };

        template<class Key, class Mapped, std::size_t Capacity, Concurrency::MapTopology Topology,
                 Hashing::AnyAlgo HashAlgorithm, class KeyEqual>
        using AssociativeStorage =
            typename AssociativeStorageType<Key, Mapped, Capacity, Topology, HashAlgorithm, KeyEqual>::type;

    } // namespace Detail

    /**
     * @brief Flat hash map whose synchronization and capacity policy are selected entirely at compile time.
     *
     * @details Concurrent topologies require lock-free atomic representations for @p Key and @p T. Lookups return
     * owning value snapshots, never pointers whose lifetime could race with erase. @ref Clear and destruction require
     * quiescence. A power-of-two extent selects fixed inline storage and performs no data-path allocation.
     * @ref std::dynamic_extent selects stable-value segments with a replaceable flat locator index for Local topology
     * and release-published copy-on-grow shards for concurrent topologies; mutation may allocate while growing.
     *
     * @par Progress
     * Concurrent lookup may wait for a writer that owns a matching @c Busy slot, and multiple-writer topologies use
     * spin serialization. Dynamic growth additionally allocates and copies a table. Consequently no concurrent
     * topology claims lock-free progress even though its key, value, state, and publication atomics are lock-free.
     */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Concurrency::MapOrder Order = Concurrency::MapOrder::Unordered,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    class ConcurrentMap {
        static constexpr bool kDynamic = Capacity == std::dynamic_extent;
        using Storage = Detail::AssociativeStorage<Key, T, Capacity, Topology, HashAlgorithm, KeyEqual>;

    public:
        using key_type = Key;
        using mapped_type = T;
        static constexpr std::size_t capacity = Capacity;
        static constexpr Concurrency::MapTopology topology = Topology;
        static constexpr Concurrency::MapOrder order = Order;

        ConcurrentMap() noexcept
            requires(!kDynamic)
        = default;
        explicit ConcurrentMap(std::size_t initialCapacity = 64) noexcept
            requires kDynamic
            : storage_{initialCapacity} {}

        [[nodiscard]] MapInsertResult TryInsert(const Key& key, const T& value) noexcept {
            return storage_.TryInsert(key, value);
        }
        [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const T& value) noexcept {
            return storage_.InsertOrAssign(key, value);
        }
        [[nodiscard]] std::optional<T> Find(const Key& key) const noexcept { return storage_.Find(key); }
        [[nodiscard, clang::always_inline]] bool TryGet(const Key& key, T& output) const noexcept {
            return storage_.TryGet(key, output);
        }
        [[nodiscard, clang::always_inline]] T* FindRef(const Key& key) noexcept
            requires(Topology.sharing == Concurrency::MapSharing::SingleThread)
        {
            return storage_.FindRef(key);
        }
        [[nodiscard]] bool Contains(const Key& key) const noexcept { return storage_.Contains(key); }
        [[nodiscard]] bool Erase(const Key& key) noexcept { return storage_.Erase(key); }
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return storage_.SizeApprox(); }
        [[nodiscard]] bool Empty() const noexcept { return storage_.Empty(); }
        /**
         * @brief Return an owning weak snapshot; Ordered mode sorts by key, Unordered mode keeps physical slot order.
         * @details Concurrent mutation may overlap individual copied entries; this operation is not a transactional
         * point-in-time snapshot.
         */
        [[nodiscard]] std::vector<std::pair<Key, T>> Snapshot() const {
            auto result = storage_.Snapshot();
            if constexpr (Order == Concurrency::MapOrder::Ordered) {
                std::ranges::sort(result, [this](const auto& left, const auto& right) {
                    return keyCompare_(left.first, right.first);
                });
            }
            return result;
        }
        /** @brief Remove all entries. @pre No lookup, mutation, snapshot, or other clear may overlap this call. */
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        SORA_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Flat hash set with compile-time reader/writer topology and capacity-policy specialization. */
    template<class Key, std::size_t Capacity, Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Concurrency::MapOrder Order = Concurrency::MapOrder::Unordered,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    class ConcurrentSet {
        static constexpr bool kDynamic = Capacity == std::dynamic_extent;
        using Storage = Detail::AssociativeStorage<Key, Detail::SetMarker, Capacity, Topology, HashAlgorithm, KeyEqual>;

    public:
        using key_type = Key;
        static constexpr std::size_t capacity = Capacity;
        static constexpr Concurrency::MapTopology topology = Topology;
        static constexpr Concurrency::MapOrder order = Order;

        ConcurrentSet() noexcept
            requires(!kDynamic)
        = default;
        explicit ConcurrentSet(std::size_t initialCapacity = 64) noexcept
            requires kDynamic
            : storage_{initialCapacity} {}

        [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept { return storage_.TryInsert(key); }
        [[nodiscard]] bool Contains(const Key& key) const noexcept { return storage_.Contains(key); }
        [[nodiscard]] bool Erase(const Key& key) noexcept { return storage_.Erase(key); }
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return storage_.SizeApprox(); }
        [[nodiscard]] bool Empty() const noexcept { return storage_.Empty(); }
        /**
         * @brief Return an owning weak snapshot; Ordered mode sorts by key, Unordered mode keeps physical slot order.
         * @details Concurrent mutation may overlap individual copied keys; this operation is not a transactional
         * point-in-time snapshot.
         */
        [[nodiscard]] std::vector<Key> Snapshot() const {
            auto result = storage_.Snapshot();
            if constexpr (Order == Concurrency::MapOrder::Ordered) {
                std::ranges::sort(result, keyCompare_);
            }
            return result;
        }
        /** @brief Remove all entries. @pre No lookup, mutation, snapshot, or other clear may overlap this call. */
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        SORA_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Explicit spelling for the unordered topology-specialized concurrent map. */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashMap = ConcurrentMap<Key, T, Capacity, Topology, Concurrency::MapOrder::Unordered, HashAlgorithm,
                                            KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for the unordered topology-specialized concurrent set. */
    template<class Key, std::size_t Capacity, Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashSet =
        ConcurrentSet<Key, Capacity, Topology, Concurrency::MapOrder::Unordered, HashAlgorithm, KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for a concurrent map whose owning snapshots are ordered by key. */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentOrderedMap =
        ConcurrentMap<Key, T, Capacity, Topology, Concurrency::MapOrder::Ordered, HashAlgorithm, KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for a concurrent set whose owning snapshots are ordered by key. */
    template<class Key, std::size_t Capacity, Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentOrderedSet =
        ConcurrentSet<Key, Capacity, Topology, Concurrency::MapOrder::Ordered, HashAlgorithm, KeyEqual, KeyCompare>;

} // namespace Sora
