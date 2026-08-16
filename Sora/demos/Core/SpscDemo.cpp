#include <Sora/Core/Concurrency/RingBuffer.h>
#include <Sora/Core/PAL/Thread.h>

#include <charconv>
#include <cstdint>
#include <string_view>
#include <thread>

using namespace Sora;
using namespace Sora::Concurrency;

namespace {

    struct Options {
        uint64_t iterations = 100;
        bool traceThreads = false;
    };

    [[nodiscard]] bool ParseOptions(int argc, char** argv, Options& options) noexcept {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--trace-threads") {
                options.traceThreads = true;
                continue;
            }
            if (argument != "--iterations" || ++index == argc) {
                return false;
            }
            const std::string_view text{argv[index]};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), options.iterations);
            if (error != std::errc{} || end != text.data() + text.size() || options.iterations == 0) {
                return false;
            }
        }
        return true;
    }

    void NameTraceThread(bool enabled, std::string_view name) {
        if (enabled) {
            [[maybe_unused]] const auto result = PAL::SetCurrentThreadName(name);
        }
    }

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!ParseOptions(argc, argv, options)) {
        return 2;
    }
    NameTraceThread(options.traceThreads, "main");

    RingBuffer<int, 8, Spsc> buffer{};

    std::thread producer([&buffer, options]() {
        NameTraceThread(options.traceThreads, "producer");
        for (uint64_t i = 0; i < options.iterations; ++i) {
            while (!buffer.TryPush(static_cast<int>(i))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buffer, options]() {
        NameTraceThread(options.traceThreads, "consumer");
        for (uint64_t i = 0; i < options.iterations; ++i) {
            int value;
            while (!buffer.TryPop(value)) {
                std::this_thread::yield();
            }
            // Process the value
        }
    });

    producer.join();
    consumer.join();

    return 0;
}
