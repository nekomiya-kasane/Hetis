#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if __has_include(<flat_map>)
#include <flat_map>
#define SORA_HAS_STD_FLAT_MAP 1
#else
#define SORA_HAS_STD_FLAT_MAP 0
#endif

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

struct SortedVectorMap {
    std::vector<std::pair<int, int>> entries;

    void reserve(std::size_t n) { entries.reserve(n); }

    void emplace(int key, int value) { entries.emplace_back(key, value); }

    void finalize() {
        std::ranges::sort(entries, {}, &std::pair<int, int>::first);
    }

    [[nodiscard]] auto find(int key) const {
        return std::ranges::lower_bound(entries, key, {}, &std::pair<int, int>::first);
    }

    [[nodiscard]] auto end() const { return entries.end(); }
};

#if SORA_HAS_STD_FLAT_MAP
using FlatMap = std::flat_map<int, int>;
constexpr std::string_view kFlatMapName = "std::flat_map";
#else
using FlatMap = SortedVectorMap;
constexpr std::string_view kFlatMapName = "sorted_vector_map_fallback";
#endif

template<typename Fn>
double time_ns_per_op(std::size_t operations, Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return static_cast<double>(ns) / static_cast<double>(operations);
}

std::vector<int> shuffled_keys(int n, std::mt19937& rng) {
    std::vector<int> keys(n);
    std::iota(keys.begin(), keys.end(), 0);
    std::ranges::shuffle(keys, rng);
    return keys;
}

template<typename Map>
std::uint64_t successful_lookup(const Map& map, const std::vector<int>& queries, std::size_t repeats) {
    std::uint64_t sum = 0;
    for (std::size_t r = 0; r < repeats; ++r) {
        for (int key : queries) {
            const auto it = map.find(key);
            if (it != map.end()) {
                sum += static_cast<std::uint64_t>(it->second);
            }
        }
    }
    return sum;
}

template<typename Map>
std::uint64_t miss_lookup(const Map& map, const std::vector<int>& queries, std::size_t repeats, int offset) {
    std::uint64_t sum = 0;
    for (std::size_t r = 0; r < repeats; ++r) {
        for (int key : queries) {
            const auto it = map.find(key + offset);
            sum += it == map.end() ? 1u : 0u;
        }
    }
    return sum;
}

template<typename Map>
std::uint64_t iterate_map(const Map& map, std::size_t repeats) {
    std::uint64_t sum = 0;
    for (std::size_t r = 0; r < repeats; ++r) {
        for (const auto& [key, value] : map) {
            sum += static_cast<std::uint64_t>(key ^ value);
        }
    }
    return sum;
}

template<typename Map>
void reserve_if_supported(Map& map, std::size_t n) {
    if constexpr (requires(Map& candidate) { candidate.reserve(n); }) {
        map.reserve(n);
    }
}

template<typename Map>
void finalize_if_supported(Map& map) {
    if constexpr (requires(Map& candidate) { candidate.finalize(); }) {
        map.finalize();
    }
}

FlatMap build_flat(const std::vector<int>& keys) {
    FlatMap map;
    reserve_if_supported(map, keys.size());
    for (int key : keys) {
        map.emplace(key, key * 17 + 3);
    }
    finalize_if_supported(map);
    return map;
}

std::unordered_map<int, int> build_unordered(const std::vector<int>& keys) {
    std::unordered_map<int, int> map;
    map.reserve(keys.size());
    for (int key : keys) {
        map.emplace(key, key * 17 + 3);
    }
    return map;
}

std::size_t repeats_for(int n) {
    return std::max<std::size_t>(128, 1'048'576 / static_cast<std::size_t>(n));
}

} // namespace

int main() {
    std::mt19937 rng{0x5eed};
    constexpr int sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 4096};

    std::cout << "container,implementation,n,operation,ns_per_op\n";

    for (int n : sizes) {
        const std::vector<int> keys = shuffled_keys(n, rng);
        const std::vector<int> queries = shuffled_keys(n, rng);
        const std::size_t repeats = repeats_for(n);

        const double flat_build = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            std::uint64_t sum = 0;
            for (std::size_t r = 0; r < repeats; ++r) {
                auto map = build_flat(keys);
                sum += static_cast<std::uint64_t>(map.find(keys.front())->second);
            }
            g_sink ^= sum;
        });
        const auto flat = build_flat(keys);
        const double flat_hit = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= successful_lookup(flat, queries, repeats);
        });
        const double flat_miss = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= miss_lookup(flat, queries, repeats, n + 1);
        });
        const double flat_iter = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= iterate_map(flat, repeats);
        });

        const double unordered_build = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            std::uint64_t sum = 0;
            for (std::size_t r = 0; r < repeats; ++r) {
                auto map = build_unordered(keys);
                sum += static_cast<std::uint64_t>(map.find(keys.front())->second);
            }
            g_sink ^= sum;
        });
        const auto unordered = build_unordered(keys);
        const double unordered_hit = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= successful_lookup(unordered, queries, repeats);
        });
        const double unordered_miss = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= miss_lookup(unordered, queries, repeats, n + 1);
        });
        const double unordered_iter = time_ns_per_op(repeats * static_cast<std::size_t>(n), [&] {
            g_sink ^= iterate_map(unordered, repeats);
        });

        std::cout << "flat_map," << kFlatMapName << ',' << n << ",build_insert," << flat_build << '\n';
        std::cout << "flat_map," << kFlatMapName << ',' << n << ",hit_lookup," << flat_hit << '\n';
        std::cout << "flat_map," << kFlatMapName << ',' << n << ",miss_lookup," << flat_miss << '\n';
        std::cout << "flat_map," << kFlatMapName << ',' << n << ",iteration," << flat_iter << '\n';
        std::cout << "unordered_map,std::unordered_map," << n << ",build_insert," << unordered_build << '\n';
        std::cout << "unordered_map,std::unordered_map," << n << ",hit_lookup," << unordered_hit << '\n';
        std::cout << "unordered_map,std::unordered_map," << n << ",miss_lookup," << unordered_miss << '\n';
        std::cout << "unordered_map,std::unordered_map," << n << ",iteration," << unordered_iter << '\n';
    }

    return static_cast<int>(g_sink == 0xdeadbeef);
}
