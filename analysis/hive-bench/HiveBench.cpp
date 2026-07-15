#include "Sora/Core/ADT/hive"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <list>
#include <memory_resource>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct alignas(32) Particle {
    std::uint64_t id;
    std::uint64_t generation;
    double x;
    double y;
};

static_assert(sizeof(Particle) == 32);

volatile std::uint64_t g_sink = 0;

constexpr std::array<std::size_t, 25> kSizes = {
    64,    96,    128,   192,   256,   384,   512,    768,    1024,
    1536,  2048,  3072,  4096,  6144,  8192,  12288,  16384,  24576,
    32768, 49152, 65536, 98304, 131072, 196608, 262144,
};

constexpr int kSamples = 5;

[[nodiscard]] constexpr auto make_particle(std::uint64_t id) noexcept -> Particle {
    return Particle{
        .id = id,
        .generation = id * 6364136223846793005ULL + 1442695040888963407ULL,
        .x = static_cast<double>(id) * 0.25,
        .y = static_cast<double>(id ^ 0x9e3779b97f4a7c15ULL) * 0.125,
    };
}

template<typename Container>
void reserve_if_supported(Container& container, std::size_t n) {
    if constexpr (requires { container.reserve(n); }) {
        container.reserve(n);
    }
}

template<typename Container>
void append(Container& container, Particle value) {
    if constexpr (requires { container.emplace_back(value); }) {
        container.emplace_back(value);
    } else {
        container.emplace(value);
    }
}

template<typename Container>
[[nodiscard]] auto build_container(std::size_t n) -> Container {
    Container container;
    reserve_if_supported(container, n);
    for (std::size_t i = 0; i != n; ++i) {
        append(container, make_particle(i));
    }
    return container;
}

template<typename Container>
[[nodiscard]] auto build_container(std::size_t n, std::pmr::memory_resource* resource) -> Container {
    Container container{std::pmr::polymorphic_allocator<typename Container::value_type>{resource}};
    reserve_if_supported(container, n);
    for (std::size_t i = 0; i != n; ++i) {
        append(container, make_particle(i));
    }
    return container;
}

template<typename Container>
[[nodiscard]] auto sum_container(const Container& container) -> std::uint64_t {
    std::uint64_t sum = 0;
    for (const Particle& particle : container) {
        sum += particle.id ^ particle.generation;
    }
    return sum;
}

[[nodiscard]] auto repeats_for(std::string_view operation, std::size_t n) -> int {
    if (operation == "build_insert") {
        return static_cast<int>(std::clamp<std::size_t>(128'000 / n, 1, 512));
    }
    if (operation == "iterate_sum") {
        return static_cast<int>(std::clamp<std::size_t>(4'000'000 / n, 2, 16384));
    }
    if (operation == "erase_if_25pct") {
        return static_cast<int>(std::clamp<std::size_t>(128'000 / n, 1, 256));
    }
    return static_cast<int>(std::clamp<std::size_t>(128'000 / n, 1, 256));
}

[[nodiscard]] auto pmr_buffer_size(std::size_t n) -> std::size_t {
    return std::max<std::size_t>(1 << 20, n * (sizeof(Particle) + 128));
}

template<typename Container>
void run_regular_case(std::string_view container_name) {
    for (std::size_t n : kSizes) {
        for (int sample = 0; sample != kSamples; ++sample) {
            const int build_repeats = repeats_for("build_insert", n);
            std::uint64_t checksum = 0;
            std::int64_t total_ns = 0;
            for (int repeat = 0; repeat != build_repeats; ++repeat) {
                auto start = Clock::now();
                auto container = build_container<Container>(n);
                auto end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += sum_container(container);
            }
            auto ns = total_ns;
            g_sink ^= checksum;
            std::cout << container_name << ",build_insert," << n << ',' << sample << ',' << build_repeats << ','
                      << (n * static_cast<std::size_t>(build_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(build_repeats))
                      << ',' << checksum << '\n';

            auto container = build_container<Container>(n);
            const int iterate_repeats = repeats_for("iterate_sum", n);
            auto start = Clock::now();
            checksum = 0;
            for (int repeat = 0; repeat != iterate_repeats; ++repeat) {
                checksum += sum_container(container);
            }
            auto end = Clock::now();
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            g_sink ^= checksum;
            std::cout << container_name << ",iterate_sum," << n << ',' << sample << ',' << iterate_repeats << ','
                      << (n * static_cast<std::size_t>(iterate_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(iterate_repeats))
                      << ',' << checksum << '\n';

            const int erase_repeats = repeats_for("erase_if_25pct", n);
            checksum = 0;
            total_ns = 0;
            for (int repeat = 0; repeat != erase_repeats; ++repeat) {
                auto erased = build_container<Container>(n);
                start = Clock::now();
                const auto erased_count =
                    std::erase_if(erased, [](const Particle& particle) { return (particle.id & 3ULL) == 0; });
                end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += erased_count + sum_container(erased);
            }
            ns = total_ns;
            g_sink ^= checksum;
            std::cout << container_name << ",erase_if_25pct," << n << ',' << sample << ',' << erase_repeats << ','
                      << (n * static_cast<std::size_t>(erase_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(erase_repeats))
                      << ',' << checksum << '\n';

            const int churn_repeats = repeats_for("churn_erase_insert_25pct", n);
            checksum = 0;
            total_ns = 0;
            for (int repeat = 0; repeat != churn_repeats; ++repeat) {
                auto churned = build_container<Container>(n);
                start = Clock::now();
                const auto erased = std::erase_if(churned, [](const Particle& particle) {
                    return (particle.id & 3ULL) == 1;
                });
                for (std::size_t i = 0; i != erased; ++i) {
                    append(churned, make_particle(n + i + static_cast<std::size_t>(repeat) * n));
                }
                end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += erased + sum_container(churned);
            }
            ns = total_ns;
            g_sink ^= checksum;
            const std::size_t mutations = (n / 2) * static_cast<std::size_t>(churn_repeats);
            std::cout << container_name << ",churn_erase_insert_25pct," << n << ',' << sample << ','
                      << churn_repeats << ',' << mutations << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(mutations) << ',' << checksum << '\n';
        }
    }
}

template<typename Container>
void run_pmr_case(std::string_view container_name) {
    for (std::size_t n : kSizes) {
        for (int sample = 0; sample != kSamples; ++sample) {
            const int build_repeats = repeats_for("build_insert", n);
            std::uint64_t checksum = 0;
            std::int64_t total_ns = 0;
            for (int repeat = 0; repeat != build_repeats; ++repeat) {
                std::vector<std::byte> buffer(pmr_buffer_size(n));
                std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size());
                auto start = Clock::now();
                auto container = build_container<Container>(n, &resource);
                auto end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += sum_container(container);
            }
            auto ns = total_ns;
            g_sink ^= checksum;
            std::cout << container_name << ",build_insert," << n << ',' << sample << ',' << build_repeats << ','
                      << (n * static_cast<std::size_t>(build_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(build_repeats))
                      << ',' << checksum << '\n';

            std::vector<std::byte> iterate_buffer(pmr_buffer_size(n));
            std::pmr::monotonic_buffer_resource iterate_resource(iterate_buffer.data(), iterate_buffer.size());
            auto container = build_container<Container>(n, &iterate_resource);
            const int iterate_repeats = repeats_for("iterate_sum", n);
            auto start = Clock::now();
            checksum = 0;
            for (int repeat = 0; repeat != iterate_repeats; ++repeat) {
                checksum += sum_container(container);
            }
            auto end = Clock::now();
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            g_sink ^= checksum;
            std::cout << container_name << ",iterate_sum," << n << ',' << sample << ',' << iterate_repeats << ','
                      << (n * static_cast<std::size_t>(iterate_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(iterate_repeats))
                      << ',' << checksum << '\n';

            const int erase_repeats = repeats_for("erase_if_25pct", n);
            checksum = 0;
            total_ns = 0;
            for (int repeat = 0; repeat != erase_repeats; ++repeat) {
                std::vector<std::byte> buffer(pmr_buffer_size(n));
                std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size());
                auto erased = build_container<Container>(n, &resource);
                start = Clock::now();
                const auto erased_count =
                    std::erase_if(erased, [](const Particle& particle) { return (particle.id & 3ULL) == 0; });
                end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += erased_count + sum_container(erased);
            }
            ns = total_ns;
            g_sink ^= checksum;
            std::cout << container_name << ",erase_if_25pct," << n << ',' << sample << ',' << erase_repeats << ','
                      << (n * static_cast<std::size_t>(erase_repeats)) << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(n * static_cast<std::size_t>(erase_repeats))
                      << ',' << checksum << '\n';

            const int churn_repeats = repeats_for("churn_erase_insert_25pct", n);
            checksum = 0;
            total_ns = 0;
            for (int repeat = 0; repeat != churn_repeats; ++repeat) {
                std::vector<std::byte> buffer(pmr_buffer_size(n));
                std::pmr::monotonic_buffer_resource resource(buffer.data(), buffer.size());
                auto churned = build_container<Container>(n, &resource);
                start = Clock::now();
                const auto erased = std::erase_if(churned, [](const Particle& particle) {
                    return (particle.id & 3ULL) == 1;
                });
                for (std::size_t i = 0; i != erased; ++i) {
                    append(churned, make_particle(n + i + static_cast<std::size_t>(repeat) * n));
                }
                end = Clock::now();
                total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                checksum += erased + sum_container(churned);
            }
            ns = total_ns;
            g_sink ^= checksum;
            const std::size_t mutations = (n / 2) * static_cast<std::size_t>(churn_repeats);
            std::cout << container_name << ",churn_erase_insert_25pct," << n << ',' << sample << ','
                      << churn_repeats << ',' << mutations << ',' << ns << ','
                      << static_cast<double>(ns) / static_cast<double>(mutations) << ',' << checksum << '\n';
        }
    }
}

} // namespace

int main() {
    std::cout << "container,operation,n,sample,repeats,operations,total_ns,ns_per_op,checksum\n";

    run_regular_case<std::vector<Particle>>("std::vector");
    run_regular_case<std::deque<Particle>>("std::deque");
    run_regular_case<std::list<Particle>>("std::list");
    run_regular_case<std::hive<Particle>>("std::hive");

    run_pmr_case<std::pmr::vector<Particle>>("std::pmr::vector");
    run_pmr_case<std::pmr::deque<Particle>>("std::pmr::deque");
    run_pmr_case<std::pmr::list<Particle>>("std::pmr::list");
    run_pmr_case<std::pmr::hive<Particle>>("std::pmr::hive");

    return static_cast<int>(g_sink == 0x123456789abcdef0ULL);
}
