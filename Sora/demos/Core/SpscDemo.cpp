#include <Sora/Core/Concurrency/SpscRingBuffer.h>

#include <thread>

using namespace Sora;
using namespace Sora::Concurrency;

int main() {
    SpscRingBuffer<int, 8> buffer{};

    std::thread producer([&buffer]() {
        for (int i = 0; i < 100; ++i) {
            while (!buffer.TryPush(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&buffer]() {
        for (int i = 0; i < 100; ++i) {
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