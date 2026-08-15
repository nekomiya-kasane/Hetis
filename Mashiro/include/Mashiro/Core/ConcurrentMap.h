/**
 * @file ConcurrentMap.h
 * @brief Fixed-capacity flat concurrent map and set specialized by compile-time reader/writer topology.
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/Attributes.h"
#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/Hash.h"
#include "Mashiro/Core/MapTopology.h"

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

namespace Mashiro {

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
            Values,
            WriterStripes,
        };

        template<ConcurrentMapField Field>
        using EmptyConcurrentMapField = std::integral_constant<ConcurrentMapField, Field>;

        template<class T>
        class RawMapSlot {
        public:
            RawMapSlot() noexcept = default;

            [[nodiscard]] void* Storage() noexcept { return storage_; }
            [[nodiscard]] T* Get() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
            [[nodiscard]] const T* Get() const noexcept {
                return std::launder(reinterpret_cast<const T*>(storage_));
            }

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

        template<class Mapped, std::size_t Extent, bool IsSet>
        struct AssociativeValueArrayType {
            using type = AssociativeArray<RawMapSlot<Mapped>, Extent>;
        };

        template<class Mapped, std::size_t Extent>
        struct AssociativeValueArrayType<Mapped, Extent, true> {
            using type = EmptyConcurrentMapField<ConcurrentMapField::Values>;
        };

        template<class Key, class Mapped, bool IsSet>
        struct LocalMapEntry {
            std::uint64_t state{};
            RawMapSlot<Key> key;
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

        struct MapSegmentFilter {
            std::atomic<std::uint64_t> words[2]{};

            void Add(std::uint64_t hash) noexcept {
                const std::size_t firstWord = static_cast<std::size_t>(hash >> 1U) & 1U;
                const std::size_t firstBit = static_cast<std::size_t>(hash >> 7U) & 63U;
                const std::size_t secondWord = static_cast<std::size_t>(hash >> 8U) & 1U;
                const std::size_t secondBit = static_cast<std::size_t>(hash >> 14U) & 63U;
                words[firstWord].fetch_or(std::uint64_t{1} << firstBit, std::memory_order_relaxed);
                words[secondWord].fetch_or(std::uint64_t{1} << secondBit, std::memory_order_relaxed);
            }

            [[nodiscard]] bool MayContain(std::uint64_t hash) const noexcept {
                const std::size_t firstWord = static_cast<std::size_t>(hash >> 1U) & 1U;
                const std::size_t firstBit = static_cast<std::size_t>(hash >> 7U) & 63U;
                const std::size_t secondWord = static_cast<std::size_t>(hash >> 8U) & 1U;
                const std::size_t secondBit = static_cast<std::size_t>(hash >> 14U) & 63U;
                const std::uint64_t first = words[firstWord].load(std::memory_order_relaxed);
                const std::uint64_t second = words[secondWord].load(std::memory_order_relaxed);
                return (first & (std::uint64_t{1} << firstBit)) != 0 &&
                       (second & (std::uint64_t{1} << secondBit)) != 0;
            }

            void Clear() noexcept {
                words[0].store(0, std::memory_order_relaxed);
                words[1].store(0, std::memory_order_relaxed);
            }
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
            using State = std::conditional_t<kLocal, StateWord, std::atomic<StateWord>>;
            using Size = std::conditional_t<kLocal, std::size_t, std::atomic<std::size_t>>;
            using LocalEntries = AssociativeArray<LocalMapEntry<Key, Mapped, kSet>, Capacity>;
            using States = AssociativeArray<State, Capacity>;
            using KeySlots = AssociativeArray<RawMapSlot<Key>, Capacity>;
            using ValueSlots = typename AssociativeValueArrayType<Mapped, Capacity, kSet>::type;
            using DynamicCapacity = std::conditional_t<kDynamic, std::size_t,
                                                       EmptyConcurrentMapField<ConcurrentMapField::Capacity>>;
            using WriterStripes = std::conditional_t<
                kMultipleWriters, std::array<MapWriterStripe, kStripeCount>,
                EmptyConcurrentMapField<ConcurrentMapField::WriterStripes>>;

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
                requires (!kDynamic)
            = default;

            explicit FlatConcurrentAssociativeStorage(std::size_t requestedCapacity) noexcept
                requires kDynamic
                : dynamicCapacity_{std::bit_ceil(std::max<std::size_t>(requestedCapacity, 2))} {
                if constexpr (kLocal) {
                    localEntries_.reset(new (std::nothrow) LocalMapEntry<Key, Mapped, kSet>[dynamicCapacity_]{});
                } else {
                    states_.reset(new (std::nothrow) State[dynamicCapacity_]{});
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

            ~FlatConcurrentAssociativeStorage() noexcept { Clear(); }

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

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return InsertImpl(key, nullptr, false).first;
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                return InsertImpl(key, std::addressof(value), false).first;
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                const auto [insert, assigned] = InsertImpl(key, std::addressof(value), true);
                if (assigned) {
                    return MapAssignResult::Assigned;
                }
                if (insert == MapInsertResult::Inserted) {
                    return MapAssignResult::Inserted;
                }
                return MapAssignResult::Full;
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires (!kSet)
            {
                const std::uint64_t hash = HashKey(key);
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
                requires (!kSet)
            {
                const std::uint64_t hash = HashKey(key);
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

            [[nodiscard]] Mapped* FindRef(const Key& key) noexcept
                requires (kLocal && !kSet)
            {
                const std::uint64_t hash = HashKey(key);
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

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                const std::uint64_t hash = HashKey(key);
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

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                const std::uint64_t hash = HashKey(key);
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
                        Publish(index, state, MapSlotStatus::Tombstone, 0);
                        DecrementSize();
                        return true;
                    }
                    if (!retry) {
                        return false;
                    }
                }
            }

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
             * @brief Copy currently published map entries in physical slot order.
             * @details Snapshot allocation is intentionally outside the hot path.
             */
            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires (!kSet)
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
                        std::destroy_at(KeyPointer(index));
                        if constexpr (!kSet) {
                            std::destroy_at(ValuePointer(index));
                        }
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
                const StateWord busy = Encode(Generation(expected), MapSlotStatus::Busy, fingerprint);
                if constexpr (kMultipleWriters) {
                    return states_[index].compare_exchange_strong(expected, busy, std::memory_order_acq_rel,
                                                                  std::memory_order_acquire);
                } else {
                    StoreState(index, busy, std::memory_order_release);
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
                requires (!kSet)
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
                requires (!kSet)
            {
                if constexpr (kLocal) {
                    *localEntries_[index].value.Get() = value;
                } else {
                    std::atomic_ref<Mapped>{*values_[index].Get()}.store(value, std::memory_order_relaxed);
                }
            }

            [[nodiscard]] MapWriterStripe* SelectWriterStripe(std::uint64_t hash) noexcept {
                if constexpr (kMultipleWriters) {
                    return std::addressof(writerStripes_[static_cast<std::size_t>(hash >> 32) & (kStripeCount - 1)]);
                } else {
                    static_cast<void>(hash);
                    return nullptr;
                }
            }

            void IncrementSize() noexcept {
                if constexpr (kLocal) {
                    ++size_;
                } else {
                    size_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            void DecrementSize() noexcept {
                if constexpr (kLocal) {
                    --size_;
                } else {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            [[nodiscard]] std::pair<MapInsertResult, bool> InsertImpl(const Key& key, const Mapped* value,
                                                                      bool assign) noexcept {
                const std::uint64_t hash = HashKey(key);
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
                            return {MapInsertResult::Inserted, false};
                        }
                    }

                    if (retry) {
                        continue;
                    }

                    if (tombstone != CapacityLimit()) {
                        if (InsertAt(tombstone, tombstoneState, fingerprint, key, value)) {
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
                    std::construct_at(KeyPointer(index), key);
                    if constexpr (!kSet) {
                        std::construct_at(ValuePointer(index), *value);
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

            [[nodiscard]] const Key* KeyPointer(std::size_t index) const noexcept {
                if constexpr (kLocal) {
                    return localEntries_[index].key.Get();
                } else {
                    return keys_[index].Get();
                }
            }

            [[nodiscard]] Mapped* ValuePointer(std::size_t index) noexcept
                requires (!kSet)
            {
                if constexpr (kLocal) {
                    return localEntries_[index].value.Get();
                } else {
                    return values_[index].Get();
                }
            }

            MASHIRO_NO_UNIQUE_ADDRESS DynamicCapacity dynamicCapacity_{};
            MASHIRO_NO_UNIQUE_ADDRESS LocalEntries localEntries_{};
            alignas(Platform::kCacheLineSize) States states_{};
            alignas(Platform::kCacheLineSize) KeySlots keys_{};
            MASHIRO_NO_UNIQUE_ADDRESS ValueSlots values_{};
            alignas(Platform::kCacheLineSize) Size size_{};
            MASHIRO_NO_UNIQUE_ADDRESS WriterStripes writerStripes_{};
            MASHIRO_NO_UNIQUE_ADDRESS HashAlgorithm hashAlgorithm_{};
            MASHIRO_NO_UNIQUE_ADDRESS KeyEqual keyEqual_{};
        };

        /**
         * @brief Dynamically growing segment directory over the same versioned flat-table protocol.
         *
         * @details Published segments are immutable as directory nodes and remain alive until quiescent destruction.
         * Growth therefore needs no reader registration, hazard publication, refcount, or stop-the-world rehash. New
         * segments double geometrically; lookups search newest first. Equal-key mutations are serialized by a global
         * hash stripe so a key cannot be published in two segments.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class DynamicConcurrentAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static constexpr bool kLocal = Topology.sharing == Concurrency::MapSharing::SingleThread;
            static constexpr bool kMultipleWriters = Topology.writers == Concurrency::MapCardinality::Multiple;
            static constexpr std::size_t kWriterStripeCount = 64;
            using SegmentStorage = FlatConcurrentAssociativeStorage<Key, Mapped, std::dynamic_extent, Topology,
                                                                       HashAlgorithm, KeyEqual>;

            struct Segment {
                explicit Segment(std::size_t capacity, Segment* older) noexcept : storage{capacity}, next{older} {}

                SegmentStorage storage;
                Segment* next;
                MapSegmentFilter filter;
            };

            using Head = std::conditional_t<kLocal, Segment*, std::atomic<Segment*>>;
            using Size = std::conditional_t<kLocal, std::size_t, std::atomic<std::size_t>>;
            using WriterStripes = std::conditional_t<
                kMultipleWriters, std::array<MapWriterStripe, kWriterStripeCount>,
                EmptyConcurrentMapField<ConcurrentMapField::WriterStripes>>;

        public:
            explicit DynamicConcurrentAssociativeStorage(std::size_t initialCapacity = 64) noexcept
                : initialCapacity_{NormalizeCapacity(initialCapacity)} {
                StoreHead(CreateSegment(initialCapacity_, nullptr), std::memory_order_relaxed);
            }

            DynamicConcurrentAssociativeStorage(const DynamicConcurrentAssociativeStorage&) = delete;
            DynamicConcurrentAssociativeStorage& operator=(const DynamicConcurrentAssociativeStorage&) = delete;
            DynamicConcurrentAssociativeStorage(DynamicConcurrentAssociativeStorage&&) = delete;
            DynamicConcurrentAssociativeStorage& operator=(DynamicConcurrentAssociativeStorage&&) = delete;

            ~DynamicConcurrentAssociativeStorage() noexcept { DestroySegments(LoadHead(std::memory_order_relaxed)); }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return InsertSet(key);
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                const std::uint64_t hash = HashKey(key);
                MapWriterGuard writer{SelectWriterStripe(hash)};
                for (;;) {
                    Segment* head = LoadHead(std::memory_order_acquire);
                    if (!head) {
                        if (!Grow(nullptr)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    for (Segment* segment = head->next; segment; segment = segment->next) {
                        if (segment->filter.MayContain(hash) && segment->storage.Contains(key)) {
                            return MapInsertResult::Exists;
                        }
                    }
                    const MapInsertResult result = head->storage.TryInsert(key, value);
                    if (result == MapInsertResult::Inserted) {
                        head->filter.Add(hash);
                        IncrementSize();
                        return result;
                    }
                    if (result == MapInsertResult::Exists) {
                        return result;
                    }
                    if (result == MapInsertResult::Full) {
                        if (!Grow(head)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    return result;
                }
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                const std::uint64_t hash = HashKey(key);
                MapWriterGuard writer{SelectWriterStripe(hash)};
                for (;;) {
                    Segment* head = LoadHead(std::memory_order_acquire);
                    if (!head) {
                        if (!Grow(nullptr)) {
                            return MapAssignResult::AllocationFailed;
                        }
                        continue;
                    }
                    for (Segment* segment = head->next; segment; segment = segment->next) {
                        if (segment->filter.MayContain(hash) && segment->storage.Contains(key)) {
                            return segment->storage.InsertOrAssign(key, value);
                        }
                    }
                    const MapAssignResult result = head->storage.InsertOrAssign(key, value);
                    if (result == MapAssignResult::Inserted) {
                        head->filter.Add(hash);
                        IncrementSize();
                        return result;
                    }
                    if (result == MapAssignResult::Assigned) {
                        return result;
                    }
                    if (result == MapAssignResult::Full) {
                        if (!Grow(head)) {
                            return MapAssignResult::AllocationFailed;
                        }
                        continue;
                    }
                    return result;
                }
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires (!kSet)
            {
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    if (auto value = segment->storage.Find(key)) {
                        return value;
                    }
                }
                return std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires (!kSet)
            {
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    if (segment->storage.TryGet(key, output)) {
                        return true;
                    }
                }
                return false;
            }

            [[nodiscard]] Mapped* FindRef(const Key& key) noexcept
                requires (kLocal && !kSet)
            {
                for (Segment* segment = LoadHead(std::memory_order_relaxed); segment; segment = segment->next) {
                    if (Mapped* value = segment->storage.FindRef(key)) {
                        return value;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    if (segment->storage.Contains(key)) {
                        return true;
                    }
                }
                return false;
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                const std::uint64_t hash = HashKey(key);
                MapWriterGuard writer{SelectWriterStripe(hash)};
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    if (segment->storage.Erase(key)) {
                        DecrementSize();
                        return true;
                    }
                }
                return false;
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                if constexpr (kLocal) {
                    return size_;
                } else {
                    return size_.load(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires (!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    auto entries = segment->storage.Snapshot();
                    result.insert(result.end(), std::make_move_iterator(entries.begin()),
                                  std::make_move_iterator(entries.end()));
                }
                return result;
            }

            [[nodiscard]] std::vector<Key> Snapshot() const
                requires kSet
            {
                std::vector<Key> result;
                for (Segment* segment = LoadHead(std::memory_order_acquire); segment; segment = segment->next) {
                    auto keys = segment->storage.Snapshot();
                    result.insert(result.end(), std::make_move_iterator(keys.begin()),
                                  std::make_move_iterator(keys.end()));
                }
                return result;
            }

            /** @brief Remove all entries and retain one reusable initial segment. Requires quiescence. */
            void Clear() noexcept {
                Segment* head = LoadHead(std::memory_order_relaxed);
                if (!head) {
                    StoreHead(CreateSegment(initialCapacity_, nullptr), std::memory_order_relaxed);
                    ResetSize();
                    return;
                }
                Segment* oldest = head;
                while (oldest->next) {
                    oldest = oldest->next;
                }
                oldest->storage.Clear();
                oldest->filter.Clear();
                Segment* current = head;
                while (current != oldest) {
                    Segment* next = current->next;
                    DestroySegment(current);
                    current = next;
                }
                oldest->next = nullptr;
                StoreHead(oldest, std::memory_order_relaxed);
                ResetSize();
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
                if (!segment->storage.Valid()) {
                    std::destroy_at(segment);
                    ::operator delete(memory, std::align_val_t{alignof(Segment)});
                    return nullptr;
                }
                return segment;
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

            [[nodiscard]] Segment* LoadHead(std::memory_order order) const noexcept {
                if constexpr (kLocal) {
                    static_cast<void>(order);
                    return head_;
                } else {
                    return head_.load(order);
                }
            }

            void StoreHead(Segment* head, std::memory_order order) noexcept {
                if constexpr (kLocal) {
                    static_cast<void>(order);
                    head_ = head;
                } else {
                    head_.store(head, order);
                }
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

            [[nodiscard]] MapWriterStripe* SelectWriterStripe(std::uint64_t hash) noexcept {
                if constexpr (kMultipleWriters) {
                    return std::addressof(writerStripes_[static_cast<std::size_t>(hash >> 32) &
                                                        (kWriterStripeCount - 1)]);
                } else {
                    static_cast<void>(hash);
                    return nullptr;
                }
            }

            [[nodiscard]] MapInsertResult InsertSet(const Key& key) noexcept
                requires kSet
            {
                const std::uint64_t hash = HashKey(key);
                MapWriterGuard writer{SelectWriterStripe(hash)};
                for (;;) {
                    Segment* head = LoadHead(std::memory_order_acquire);
                    if (!head) {
                        if (!Grow(nullptr)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    for (Segment* segment = head->next; segment; segment = segment->next) {
                        if (segment->filter.MayContain(hash) && segment->storage.Contains(key)) {
                            return MapInsertResult::Exists;
                        }
                    }
                    const MapInsertResult result = head->storage.TryInsert(key);
                    if (result == MapInsertResult::Inserted) {
                        head->filter.Add(hash);
                        IncrementSize();
                        return result;
                    }
                    if (result == MapInsertResult::Exists) {
                        return result;
                    }
                    if (result == MapInsertResult::Full) {
                        if (!Grow(head)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    return result;
                }
            }

            [[nodiscard]] bool Grow(Segment* observedHead) noexcept {
                growthLock_.Lock();
                Segment* currentHead = LoadHead(std::memory_order_acquire);
                if (currentHead != observedHead) {
                    growthLock_.Unlock();
                    return true;
                }
                const std::size_t currentCapacity = currentHead ? currentHead->storage.CapacityLimit()
                                                                : initialCapacity_ / 2;
                if (currentCapacity > std::numeric_limits<std::size_t>::max() / 2) {
                    growthLock_.Unlock();
                    return false;
                }
                Segment* segment = CreateSegment(currentCapacity * 2, currentHead);
                if (!segment) {
                    growthLock_.Unlock();
                    return false;
                }
                StoreHead(segment, std::memory_order_release);
                growthLock_.Unlock();
                return true;
            }

            void IncrementSize() noexcept {
                if constexpr (kLocal) {
                    ++size_;
                } else {
                    size_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            void DecrementSize() noexcept {
                if constexpr (kLocal) {
                    --size_;
                } else {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            void ResetSize() noexcept {
                if constexpr (kLocal) {
                    size_ = 0;
                } else {
                    size_.store(0, std::memory_order_relaxed);
                }
            }

            const std::size_t initialCapacity_;
            Head head_{};
            Size size_{};
            MapWriterStripe growthLock_{};
            MASHIRO_NO_UNIQUE_ADDRESS WriterStripes writerStripes_{};
            MASHIRO_NO_UNIQUE_ADDRESS HashAlgorithm hashAlgorithm_{};
        };

        template<class Key, class Mapped, std::size_t Capacity, Concurrency::MapTopology Topology,
                 Hashing::AnyAlgo HashAlgorithm, class KeyEqual>
        struct AssociativeStorageType {
            using type = FlatConcurrentAssociativeStorage<Key, Mapped, Capacity, Topology, HashAlgorithm, KeyEqual>;
        };

        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        struct AssociativeStorageType<Key, Mapped, std::dynamic_extent, Topology, HashAlgorithm, KeyEqual> {
            using type = DynamicConcurrentAssociativeStorage<Key, Mapped, Topology, HashAlgorithm, KeyEqual>;
        };

        template<class Key, class Mapped, std::size_t Capacity, Concurrency::MapTopology Topology,
                 Hashing::AnyAlgo HashAlgorithm, class KeyEqual>
        using AssociativeStorage =
            typename AssociativeStorageType<Key, Mapped, Capacity, Topology, HashAlgorithm, KeyEqual>::type;

    } // namespace Detail

    /**
     * @brief Allocation-free flat hash map whose synchronization is selected entirely at compile time.
     *
     * @details Concurrent topologies require lock-free atomic representations for @p Key and @p T. Lookups return
     * owning value snapshots, never pointers whose lifetime could race with erase. @ref Clear and destruction require
     * quiescence. A power-of-two extent selects fixed inline storage; @ref std::dynamic_extent selects geometrically
     * growing segments whose published addresses remain stable until destruction.
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
            requires (!kDynamic)
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
        [[nodiscard]] bool TryGet(const Key& key, T& output) const noexcept { return storage_.TryGet(key, output); }
        [[nodiscard]] T* FindRef(const Key& key) noexcept
            requires (Topology.sharing == Concurrency::MapSharing::SingleThread)
        {
            return storage_.FindRef(key);
        }
        [[nodiscard]] bool Contains(const Key& key) const noexcept { return storage_.Contains(key); }
        [[nodiscard]] bool Erase(const Key& key) noexcept { return storage_.Erase(key); }
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return storage_.SizeApprox(); }
        [[nodiscard]] bool Empty() const noexcept { return storage_.Empty(); }
        /** @brief Return a stable value snapshot; Ordered mode sorts it by key, Unordered mode keeps slot order. */
        [[nodiscard]] std::vector<std::pair<Key, T>> Snapshot() const {
            auto result = storage_.Snapshot();
            if constexpr (Order == Concurrency::MapOrder::Ordered) {
                std::ranges::sort(result, [this](const auto& left, const auto& right) {
                    return keyCompare_(left.first, right.first);
                });
            }
            return result;
        }
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        MASHIRO_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Allocation-free flat hash set with compile-time reader/writer topology specialization. */
    template<class Key, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
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
            requires (!kDynamic)
        = default;
        explicit ConcurrentSet(std::size_t initialCapacity = 64) noexcept
            requires kDynamic
            : storage_{initialCapacity} {}

        [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept { return storage_.TryInsert(key); }
        [[nodiscard]] bool Contains(const Key& key) const noexcept { return storage_.Contains(key); }
        [[nodiscard]] bool Erase(const Key& key) noexcept { return storage_.Erase(key); }
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return storage_.SizeApprox(); }
        [[nodiscard]] bool Empty() const noexcept { return storage_.Empty(); }
        /** @brief Return a stable key snapshot; Ordered mode sorts it by key, Unordered mode keeps slot order. */
        [[nodiscard]] std::vector<Key> Snapshot() const {
            auto result = storage_.Snapshot();
            if constexpr (Order == Concurrency::MapOrder::Ordered) {
                std::ranges::sort(result, keyCompare_);
            }
            return result;
        }
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        MASHIRO_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Compatibility spelling for callers that name the unordered storage strategy explicitly. */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Concurrency::MapOrder Order = Concurrency::MapOrder::Unordered,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashMap =
        ConcurrentMap<Key, T, Capacity, Topology, Order, HashAlgorithm, KeyEqual, KeyCompare>;

    /** @brief Compatibility spelling for the topology-specialized concurrent set. */
    template<class Key, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Concurrency::MapOrder Order = Concurrency::MapOrder::Unordered,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashSet = ConcurrentSet<Key, Capacity, Topology, Order, HashAlgorithm, KeyEqual, KeyCompare>;

} // namespace Mashiro
