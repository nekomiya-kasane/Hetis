/**
 * @file ConcurrentMapMigrationAssembly.cpp
 * @brief Stable exported probes for inspecting concurrent-map migration code generation.
 * @ingroup Core
 */

#if defined(SORA_MIGRATION_TARGET)
#    include <Sora/Core/Concurrency/ConcurrentMap.h>
namespace Migration = Sora;
#else
#    include <Mashiro/Core/ConcurrentMap.h>
namespace Migration = Mashiro;
#endif

#include <cstddef>
#include <cstdint>
#include <span>

namespace {

    using FixedLocalMap =
        Migration::ConcurrentMap<std::uint64_t, std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Local>;
    using FixedSpscMap =
        Migration::ConcurrentMap<std::uint64_t, std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Spsc>;
    using FixedMpscMap =
        Migration::ConcurrentMap<std::uint64_t, std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Mpsc>;
    using FixedSpmcMap =
        Migration::ConcurrentMap<std::uint64_t, std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Spmc>;
    using FixedMpmcMap =
        Migration::ConcurrentMap<std::uint64_t, std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Mpmc>;

    using DynamicLocalMap = Migration::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                     Migration::Concurrency::MapTopologies::Local>;
    using DynamicSpscMap = Migration::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Migration::Concurrency::MapTopologies::Spsc>;
    using DynamicMpscMap = Migration::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Migration::Concurrency::MapTopologies::Mpsc>;
    using DynamicSpmcMap = Migration::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Migration::Concurrency::MapTopologies::Spmc>;
    using DynamicMpmcMap = Migration::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Migration::Concurrency::MapTopologies::Mpmc>;

    using FixedLocalSet = Migration::ConcurrentSet<std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Local>;
    using FixedMpmcSet = Migration::ConcurrentSet<std::uint64_t, 1024, Migration::Concurrency::MapTopologies::Mpmc>;
    using DynamicLocalSet =
        Migration::ConcurrentSet<std::uint64_t, std::dynamic_extent, Migration::Concurrency::MapTopologies::Local>;
    using DynamicMpmcSet =
        Migration::ConcurrentSet<std::uint64_t, std::dynamic_extent, Migration::Concurrency::MapTopologies::Mpmc>;

    template<typename Map>
    [[nodiscard]] std::uint8_t Insert(Map* map, std::uint64_t key, std::uint64_t value) noexcept {
        return static_cast<std::uint8_t>(map->TryInsert(key, value));
    }

    template<typename Map>
    [[nodiscard]] bool Get(const Map* map, std::uint64_t key, std::uint64_t* value) noexcept {
        return map->TryGet(key, *value);
    }

    template<typename Set>
    [[nodiscard]] std::uint8_t Insert(Set* set, std::uint64_t key) noexcept {
        return static_cast<std::uint8_t>(set->TryInsert(key));
    }

} // namespace

extern "C" {

[[gnu::noinline]] std::uint8_t MigrationFixedLocalMapInsert(FixedLocalMap* map, std::uint64_t key,
                                                            std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationFixedLocalMapGet(const FixedLocalMap* map, std::uint64_t key,
                                                 std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationFixedSpscMapInsert(FixedSpscMap* map, std::uint64_t key,
                                                           std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationFixedSpscMapGet(const FixedSpscMap* map, std::uint64_t key,
                                                std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationFixedMpscMapInsert(FixedMpscMap* map, std::uint64_t key,
                                                           std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationFixedMpscMapGet(const FixedMpscMap* map, std::uint64_t key,
                                                std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationFixedSpmcMapInsert(FixedSpmcMap* map, std::uint64_t key,
                                                           std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationFixedSpmcMapGet(const FixedSpmcMap* map, std::uint64_t key,
                                                std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationFixedMpmcMapInsert(FixedMpmcMap* map, std::uint64_t key,
                                                           std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationFixedMpmcMapGet(const FixedMpmcMap* map, std::uint64_t key,
                                                std::uint64_t* value) noexcept {
    return Get(map, key, value);
}

[[gnu::noinline]] std::uint8_t MigrationDynamicLocalMapInsert(DynamicLocalMap* map, std::uint64_t key,
                                                              std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationDynamicLocalMapGet(const DynamicLocalMap* map, std::uint64_t key,
                                                   std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicSpscMapInsert(DynamicSpscMap* map, std::uint64_t key,
                                                             std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationDynamicSpscMapGet(const DynamicSpscMap* map, std::uint64_t key,
                                                  std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicMpscMapInsert(DynamicMpscMap* map, std::uint64_t key,
                                                             std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationDynamicMpscMapGet(const DynamicMpscMap* map, std::uint64_t key,
                                                  std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicSpmcMapInsert(DynamicSpmcMap* map, std::uint64_t key,
                                                             std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationDynamicSpmcMapGet(const DynamicSpmcMap* map, std::uint64_t key,
                                                  std::uint64_t* value) noexcept {
    return Get(map, key, value);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicMpmcMapInsert(DynamicMpmcMap* map, std::uint64_t key,
                                                             std::uint64_t value) noexcept {
    return Insert(map, key, value);
}
[[gnu::noinline]] bool MigrationDynamicMpmcMapGet(const DynamicMpmcMap* map, std::uint64_t key,
                                                  std::uint64_t* value) noexcept {
    return Get(map, key, value);
}

[[gnu::noinline]] std::uint8_t MigrationFixedLocalSetInsert(FixedLocalSet* set, std::uint64_t key) noexcept {
    return Insert(set, key);
}
[[gnu::noinline]] bool MigrationFixedLocalSetContains(const FixedLocalSet* set, std::uint64_t key) noexcept {
    return set->Contains(key);
}
[[gnu::noinline]] std::uint8_t MigrationFixedMpmcSetInsert(FixedMpmcSet* set, std::uint64_t key) noexcept {
    return Insert(set, key);
}
[[gnu::noinline]] bool MigrationFixedMpmcSetContains(const FixedMpmcSet* set, std::uint64_t key) noexcept {
    return set->Contains(key);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicLocalSetInsert(DynamicLocalSet* set, std::uint64_t key) noexcept {
    return Insert(set, key);
}
[[gnu::noinline]] bool MigrationDynamicLocalSetContains(const DynamicLocalSet* set, std::uint64_t key) noexcept {
    return set->Contains(key);
}
[[gnu::noinline]] std::uint8_t MigrationDynamicMpmcSetInsert(DynamicMpmcSet* set, std::uint64_t key) noexcept {
    return Insert(set, key);
}
[[gnu::noinline]] bool MigrationDynamicMpmcSetContains(const DynamicMpmcSet* set, std::uint64_t key) noexcept {
    return set->Contains(key);
}

} // extern "C"
