#include <Sora/Core/StackTrace.h>

auto f = []() {
    [] {
        auto trace = Sora::StackTrace::Capture(0, 16);
        std::print("Captured stack trace:\n{}\n", trace);
    }();
    auto trace = Sora::StackTrace::Capture(0, 16);
    std::print("Captured stack trace:\n{}\n", trace);
};

int main() {
    f();
    return 0;
}