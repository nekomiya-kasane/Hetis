/**
 * @file ConcurrentMap.h
 * @brief Flat concurrent map and set specialized by compile-time reader/writer topology and capacity policy.
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

            [[nodiscard]] std::pair<MapInsertResult, Mapped*> TryInsertAndGet(const Key& key,
                                                                              const Mapped& value) noexcept
                requires (kLocal && !kSet)
            {
                Mapped* storedValue = nullptr;
                const MapInsertResult result = InsertImpl(key, std::addressof(value), false, &storedValue).first;
                return {result, storedValue};
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

            [[nodiscard]] std::pair<MapInsertResult, bool> InsertImpl(const Key& key, const Mapped* value, bool assign,
                                                                      Mapped** storedValue = nullptr) noexcept {
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
         * @brief Append-only local storage with a replaceable flat locator index.
         *
         * @details Keys and values remain in their original geometric segment, so references returned by FindRef stay
         * stable across growth. The replaceable index owns only duplicated keys and segment/value locators; rebuilding
         * it therefore never moves user values and reduces lookup from a historical-segment scan to one flat probe.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class IndexedDynamicLocalAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static_assert(Topology.sharing == Concurrency::MapSharing::SingleThread);

            using SegmentStorage = FlatConcurrentAssociativeStorage<Key, Mapped, std::dynamic_extent, Topology,
                                                                    HashAlgorithm, KeyEqual>;

            struct Segment;

            struct MapLocation {
                Segment* segment;
                Mapped* value;
            };

            using IndexValue = std::conditional_t<kSet, Segment*, MapLocation>;
            using IndexStorage = FlatConcurrentAssociativeStorage<Key, IndexValue, std::dynamic_extent, Topology,
                                                                  HashAlgorithm, KeyEqual>;

            struct Segment {
                explicit Segment(std::size_t capacity, Segment* older) noexcept : storage{capacity}, next{older} {}

                SegmentStorage storage;
                Segment* next;
            };

        public:
            explicit IndexedDynamicLocalAssociativeStorage(std::size_t initialCapacity = 64) noexcept
                : initialCapacity_{NormalizeCapacity(initialCapacity)}, head_{CreateSegment(initialCapacity_, nullptr)},
                  index_{CreateIndex(initialCapacity_)} {}

            IndexedDynamicLocalAssociativeStorage(const IndexedDynamicLocalAssociativeStorage&) = delete;
            IndexedDynamicLocalAssociativeStorage& operator=(const IndexedDynamicLocalAssociativeStorage&) = delete;
            IndexedDynamicLocalAssociativeStorage(IndexedDynamicLocalAssociativeStorage&&) = delete;
            IndexedDynamicLocalAssociativeStorage& operator=(IndexedDynamicLocalAssociativeStorage&&) = delete;

            ~IndexedDynamicLocalAssociativeStorage() noexcept { DestroySegments(head_); }

            [[nodiscard]] bool Valid() const noexcept { return head_ && index_ && index_->Valid(); }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                return InsertSet(key);
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                return InsertMap(key, value);
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                if (!index_) {
                    return MapAssignResult::AllocationFailed;
                }
                if (MapLocation* location = index_->FindRef(key)) {
                    *location->value = value;
                    return MapAssignResult::Assigned;
                }
                const MapInsertResult result = InsertMap(key, value);
                if (result == MapInsertResult::Inserted) {
                    return MapAssignResult::Inserted;
                }
                return result == MapInsertResult::AllocationFailed ? MapAssignResult::AllocationFailed
                                                                   : MapAssignResult::Full;
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires (!kSet)
            {
                if (!index_) {
                    return std::nullopt;
                }
                const auto location = index_->Find(key);
                return location ? std::optional<Mapped>{*location->value} : std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires (!kSet)
            {
                if (!index_) {
                    return false;
                }
                const auto location = index_->Find(key);
                if (!location) {
                    return false;
                }
                output = *location->value;
                return true;
            }

            [[nodiscard]] Mapped* FindRef(const Key& key) noexcept
                requires (!kSet)
            {
                if (!index_) {
                    return nullptr;
                }
                MapLocation* location = index_->FindRef(key);
                return location ? location->value : nullptr;
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept { return index_ && index_->Contains(key); }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                if (!index_) {
                    return false;
                }
                const auto location = index_->Find(key);
                if (!location) {
                    return false;
                }
                Segment* segment;
                if constexpr (kSet) {
                    segment = *location;
                } else {
                    segment = location->segment;
                }
                if (!segment->storage.Erase(key)) {
                    return false;
                }
                if (!index_->Erase(key)) {
                    return false;
                }
                --size_;
                return true;
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept { return size_; }
            [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires (!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                result.reserve(size_);
                for (Segment* segment = head_; segment; segment = segment->next) {
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
                result.reserve(size_);
                for (Segment* segment = head_; segment; segment = segment->next) {
                    auto keys = segment->storage.Snapshot();
                    result.insert(result.end(), std::make_move_iterator(keys.begin()),
                                  std::make_move_iterator(keys.end()));
                }
                return result;
            }

            /** @brief Remove all entries while retaining the oldest segment and current index allocations. */
            void Clear() noexcept {
                if (!head_) {
                    head_ = CreateSegment(initialCapacity_, nullptr);
                } else {
                    Segment* oldest = head_;
                    while (oldest->next) {
                        oldest = oldest->next;
                    }
                    oldest->storage.Clear();
                    Segment* current = head_;
                    while (current != oldest) {
                        Segment* next = current->next;
                        DestroySegment(current);
                        current = next;
                    }
                    oldest->next = nullptr;
                    head_ = oldest;
                }
                if (index_) {
                    index_->Clear();
                } else {
                    index_ = CreateIndex(initialCapacity_);
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
                if (!segment->storage.Valid()) {
                    std::destroy_at(segment);
                    ::operator delete(memory, std::align_val_t{alignof(Segment)});
                    return nullptr;
                }
                return segment;
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

            [[nodiscard]] bool RebuildIndex(std::size_t capacity) noexcept {
                auto replacement = CreateIndex(capacity);
                if (!replacement || !index_->CopyPublishedTo(*replacement)) {
                    return false;
                }
                index_ = std::move(replacement);
                return true;
            }

            [[nodiscard]] bool EnsureIndexCapacity() noexcept {
                if (!index_) {
                    index_ = CreateIndex(initialCapacity_);
                    return index_ != nullptr;
                }
                const std::size_t capacity = index_->CapacityLimit();
                if (size_ < capacity - capacity / 4) {
                    return true;
                }
                return capacity <= std::numeric_limits<std::size_t>::max() / 2 && RebuildIndex(capacity * 2);
            }

            [[nodiscard]] bool EnsureSegmentCapacity() noexcept {
                if (!head_) {
                    head_ = CreateSegment(initialCapacity_, nullptr);
                    return head_ != nullptr;
                }
                const std::size_t capacity = head_->storage.CapacityLimit();
                if (head_->storage.SizeApprox() < capacity - capacity / 4) {
                    return true;
                }
                if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                    return false;
                }
                Segment* segment = CreateSegment(capacity * 2, head_);
                if (!segment) {
                    return false;
                }
                head_ = segment;
                return true;
            }

            [[nodiscard]] MapInsertResult InsertIndex(const Key& key, const IndexValue& location) noexcept {
                MapInsertResult result = index_->TryInsert(key, location);
                if (result != MapInsertResult::Full) {
                    return result;
                }
                const std::size_t capacity = index_->CapacityLimit();
                if (capacity > std::numeric_limits<std::size_t>::max() / 2 || !RebuildIndex(capacity * 2)) {
                    return MapInsertResult::AllocationFailed;
                }
                return index_->TryInsert(key, location);
            }

            [[nodiscard]] MapInsertResult InsertSet(const Key& key) noexcept
                requires kSet
            {
                if (!index_) {
                    return MapInsertResult::AllocationFailed;
                }
                if (!EnsureIndexCapacity() || !EnsureSegmentCapacity()) {
                    return MapInsertResult::AllocationFailed;
                }
                const MapInsertResult result = head_->storage.TryInsert(key);
                if (result != MapInsertResult::Inserted) {
                    return result;
                }
                const MapInsertResult indexResult = InsertIndex(key, head_);
                if (indexResult != MapInsertResult::Inserted) {
                    static_cast<void>(head_->storage.Erase(key));
                    return indexResult;
                }
                ++size_;
                return MapInsertResult::Inserted;
            }

            [[nodiscard]] MapInsertResult InsertMap(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                if (!index_) {
                    return MapInsertResult::AllocationFailed;
                }
                if (!EnsureIndexCapacity() || !EnsureSegmentCapacity()) {
                    return MapInsertResult::AllocationFailed;
                }
                const auto [result, storedValue] = head_->storage.TryInsertAndGet(key, value);
                if (result != MapInsertResult::Inserted) {
                    return result;
                }
                if (!storedValue) {
                    static_cast<void>(head_->storage.Erase(key));
                    return MapInsertResult::AllocationFailed;
                }
                const MapInsertResult indexResult = InsertIndex(key, MapLocation{head_, storedValue});
                if (indexResult != MapInsertResult::Inserted) {
                    static_cast<void>(head_->storage.Erase(key));
                    return indexResult;
                }
                ++size_;
                return MapInsertResult::Inserted;
            }

            const std::size_t initialCapacity_;
            Segment* head_{};
            std::unique_ptr<IndexStorage> index_;
            std::size_t size_{};
        };

        /**
         * @brief Read-optimized dynamic shard using release-published copy-on-grow table versions.
         *
         * @details The shard serializes its writers, so its flat table is compiled as a single-writer data plane even
         * when the public topology permits multiple writers. Growth copies the current table into a table with twice
         * the capacity and publishes it with one release store. Readers acquire one version and probe only that table.
         * Retired versions remain alive until quiescent clear or destruction. Retained table storage stays below twice
         * the current capacity without reader registration or reclamation traffic on the lookup path.
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

            using WriterStripes = std::conditional_t<
                kMultipleWriters, std::array<MapWriterStripe, kWriterStripeCount>,
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

            [[nodiscard]] bool Valid() const noexcept {
                return current_.load(std::memory_order_relaxed) != nullptr;
            }

            [[nodiscard]] std::uint64_t HashForKey(const Key& key) const noexcept { return HashKey(key); }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapInsertResult::AllocationFailed;
                    }
                    if (ShouldGrow(current)) {
                        if (current->storage.Contains(key)) {
                            return MapInsertResult::Exists;
                        }
                        if (!Grow(current)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    const MapInsertResult result = current->storage.TryInsert(key);
                    if (result != MapInsertResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapInsertResult::AllocationFailed;
                    }
                    if (ShouldGrow(current)) {
                        if (current->storage.Contains(key)) {
                            return MapInsertResult::Exists;
                        }
                        if (!Grow(current)) {
                            return MapInsertResult::AllocationFailed;
                        }
                        continue;
                    }
                    const MapInsertResult result = current->storage.TryInsert(key, value);
                    if (result != MapInsertResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapInsertResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                MapWriterGuard writer{SelectWriterStripe()};
                for (;;) {
                    Version* current = LoadCurrent();
                    if (!current) {
                        return MapAssignResult::AllocationFailed;
                    }
                    if (ShouldGrow(current)) {
                        if (current->storage.Contains(key)) {
                            return current->storage.InsertOrAssign(key, value);
                        }
                        if (!Grow(current)) {
                            return MapAssignResult::AllocationFailed;
                        }
                        continue;
                    }
                    const MapAssignResult result = current->storage.InsertOrAssign(key, value);
                    if (result != MapAssignResult::Full) {
                        return result;
                    }
                    if (!Grow(current)) {
                        return MapAssignResult::AllocationFailed;
                    }
                }
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires (!kSet)
            {
                Version* current = LoadCurrent();
                return current ? current->storage.Find(key) : std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires (!kSet)
            {
                Version* current = LoadCurrent();
                return current && current->storage.TryGet(key, output);
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                Version* current = LoadCurrent();
                return current && current->storage.Contains(key);
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                MapWriterGuard writer{SelectWriterStripe()};
                Version* current = LoadCurrent();
                return current && current->storage.Erase(key);
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                Version* current = LoadCurrent();
                return current ? current->storage.SizeApprox() : 0;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires (!kSet)
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

            [[nodiscard]] Version* LoadCurrent() const noexcept {
                return current_.load(std::memory_order_acquire);
            }

            [[nodiscard]] MapWriterStripe* SelectWriterStripe() noexcept {
                if constexpr (kMultipleWriters) {
                    return std::addressof(writerStripes_[0]);
                } else {
                    return nullptr;
                }
            }

            [[nodiscard]] bool Grow(Version* current) noexcept {
                const std::size_t capacity = current->storage.CapacityLimit();
                if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                    return false;
                }
                Version* next = CreateVersion(capacity * 2, current);
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
            MASHIRO_NO_UNIQUE_ADDRESS WriterStripes writerStripes_{};
            MASHIRO_NO_UNIQUE_ADDRESS HashAlgorithm hashAlgorithm_{};
        };

        /**
         * @brief Hash-sharded dynamic storage with one release-published flat table per shard.
         *
         * @details The compile-time shard count restores writer parallelism while each shard compiles to a
         * single-writer copy-on-grow data plane. Lookups select one shard, acquire one version, and probe one flat
         * table.
         */
        template<class Key, class Mapped, Concurrency::MapTopology Topology, Hashing::AnyAlgo HashAlgorithm,
                 class KeyEqual>
        class ShardedDynamicConcurrentAssociativeStorage {
            static constexpr bool kSet = std::same_as<Mapped, SetMarker>;
            static constexpr bool kLocal = Topology.sharing == Concurrency::MapSharing::SingleThread;
            static constexpr std::size_t kShardCount = 16;
            using ShardStorage = VersionedConcurrentAssociativeStorage<Key, Mapped, Topology, HashAlgorithm, KeyEqual>;

        public:
            explicit ShardedDynamicConcurrentAssociativeStorage(std::size_t initialCapacity = 64) noexcept {
                const std::size_t shardCapacity = ShardInitialCapacity(initialCapacity);
                for (auto& shard : shards_) {
                    shard.reset(new (std::nothrow) ShardStorage(shardCapacity));
                    if (!shard || !shard->Valid()) {
                        valid_ = false;
                    }
                }
            }

            ShardedDynamicConcurrentAssociativeStorage(const ShardedDynamicConcurrentAssociativeStorage&) = delete;
            ShardedDynamicConcurrentAssociativeStorage& operator=(
                const ShardedDynamicConcurrentAssociativeStorage&) = delete;
            ShardedDynamicConcurrentAssociativeStorage(ShardedDynamicConcurrentAssociativeStorage&&) = delete;
            ShardedDynamicConcurrentAssociativeStorage& operator=(
                ShardedDynamicConcurrentAssociativeStorage&&) = delete;

            [[nodiscard]] bool Valid() const noexcept { return valid_; }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key) noexcept
                requires kSet
            {
                ShardStorage* shard = SelectShard(key);
                return shard ? shard->TryInsert(key) : MapInsertResult::AllocationFailed;
            }

            [[nodiscard]] MapInsertResult TryInsert(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                ShardStorage* shard = SelectShard(key);
                return shard ? shard->TryInsert(key, value) : MapInsertResult::AllocationFailed;
            }

            [[nodiscard]] MapAssignResult InsertOrAssign(const Key& key, const Mapped& value) noexcept
                requires (!kSet)
            {
                ShardStorage* shard = SelectShard(key);
                return shard ? shard->InsertOrAssign(key, value) : MapAssignResult::AllocationFailed;
            }

            [[nodiscard]] std::optional<Mapped> Find(const Key& key) const noexcept
                requires (!kSet)
            {
                const ShardStorage* shard = SelectShard(key);
                return shard ? shard->Find(key) : std::nullopt;
            }

            [[nodiscard]] bool TryGet(const Key& key, Mapped& output) const noexcept
                requires (!kSet)
            {
                const ShardStorage* shard = SelectShard(key);
                return shard && shard->TryGet(key, output);
            }

            [[nodiscard]] Mapped* FindRef(const Key& key) noexcept
                requires (kLocal && !kSet)
            {
                ShardStorage* shard = SelectShard(key);
                return shard ? shard->FindRef(key) : nullptr;
            }

            [[nodiscard]] bool Contains(const Key& key) const noexcept {
                const ShardStorage* shard = SelectShard(key);
                return shard && shard->Contains(key);
            }

            [[nodiscard]] bool Erase(const Key& key) noexcept {
                ShardStorage* shard = SelectShard(key);
                return shard && shard->Erase(key);
            }

            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                std::size_t size = 0;
                for (const auto& shard : shards_) {
                    if (shard) {
                        size += shard->SizeApprox();
                    }
                }
                return size;
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

            [[nodiscard]] std::vector<std::pair<Key, Mapped>> Snapshot() const
                requires (!kSet)
            {
                std::vector<std::pair<Key, Mapped>> result;
                result.reserve(SizeApprox());
                for (const auto& shard : shards_) {
                    if (!shard) {
                        continue;
                    }
                    auto entries = shard->Snapshot();
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
                    if (!shard) {
                        continue;
                    }
                    auto keys = shard->Snapshot();
                    result.insert(result.end(), std::make_move_iterator(keys.begin()),
                                  std::make_move_iterator(keys.end()));
                }
                return result;
            }

            void Clear() noexcept {
                for (auto& shard : shards_) {
                    if (shard) {
                        shard->Clear();
                    }
                }
            }

        private:
            static_assert(std::has_single_bit(kShardCount));

            [[nodiscard]] static constexpr std::size_t ShardInitialCapacity(std::size_t totalCapacity) noexcept {
                return std::max<std::size_t>(
                    totalCapacity / kShardCount + static_cast<std::size_t>(totalCapacity % kShardCount != 0), 2);
            }

            [[nodiscard]] std::size_t SelectShardIndex(const Key& key) const noexcept {
                constexpr unsigned kShardBits = std::countr_zero(kShardCount);
                constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
                return static_cast<std::size_t>((shards_[0]->HashForKey(key) * kGoldenRatio) >>
                                                (64U - kShardBits));
            }

            [[nodiscard]] ShardStorage* SelectShard(const Key& key) noexcept {
                if (!valid_) {
                    return nullptr;
                }
                return shards_[SelectShardIndex(key)].get();
            }

            [[nodiscard]] const ShardStorage* SelectShard(const Key& key) const noexcept {
                if (!valid_) {
                    return nullptr;
                }
                return shards_[SelectShardIndex(key)].get();
            }

            std::array<std::unique_ptr<ShardStorage>, kShardCount> shards_{};
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
     * @brief Allocation-free flat hash map whose synchronization is selected entirely at compile time.
     *
     * @details Concurrent topologies require lock-free atomic representations for @p Key and @p T. Lookups return
     * owning value snapshots, never pointers whose lifetime could race with erase. @ref Clear and destruction require
     * quiescence. A power-of-two extent selects fixed inline storage. @ref std::dynamic_extent selects stable-value
     * segments with a replaceable flat locator index for Local topology and release-published copy-on-grow shards for
     * concurrent topologies.
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
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        MASHIRO_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Flat hash set with compile-time reader/writer topology and capacity-policy specialization. */
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
        void Clear() noexcept { storage_.Clear(); }

    private:
        Storage storage_{};
        MASHIRO_NO_UNIQUE_ADDRESS KeyCompare keyCompare_{};
    };

    /** @brief Explicit spelling for the unordered topology-specialized concurrent map. */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashMap = ConcurrentMap<Key, T, Capacity, Topology, Concurrency::MapOrder::Unordered,
                                            HashAlgorithm, KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for the unordered topology-specialized concurrent set. */
    template<class Key, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentHashSet = ConcurrentSet<Key, Capacity, Topology, Concurrency::MapOrder::Unordered, HashAlgorithm,
                                            KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for a concurrent map whose owning snapshots are ordered by key. */
    template<class Key, class T, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentOrderedMap = ConcurrentMap<Key, T, Capacity, Topology, Concurrency::MapOrder::Ordered,
                                               HashAlgorithm, KeyEqual, KeyCompare>;

    /** @brief Explicit spelling for a concurrent set whose owning snapshots are ordered by key. */
    template<class Key, std::size_t Capacity,
             Concurrency::MapTopology Topology = Concurrency::MapTopologies::Mpmc,
             Hashing::AnyAlgo HashAlgorithm = Hashing::DefaultAlgo, class KeyEqual = std::equal_to<Key>,
             class KeyCompare = std::less<Key>>
    using ConcurrentOrderedSet = ConcurrentSet<Key, Capacity, Topology, Concurrency::MapOrder::Ordered, HashAlgorithm,
                                               KeyEqual, KeyCompare>;

} // namespace Mashiro
