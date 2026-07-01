#include <stdexec/execution.hpp>

#include <string>
#include <iostream>

stdexec::task<int> Compute(int seed) {
    auto [graph] = stdexec::sync_wait(stdexec::just(seed)).value();
    graph *= 2;
    co_return graph;
}

int main() {
    auto task = stdexec::just(1) | stdexec::then([](int x) { return std::to_string(x * 20); }) |
                stdexec::then([](auto s) { return s.size(); }) |
                stdexec::then([](auto size) { throw std::runtime_error("bad"); });

    auto res = stdexec::sync_wait(task).value(); // should be 1
    auto ss = stdexec::sync_wait(Compute(1)).value(); // should be 2
    return 0;
}
