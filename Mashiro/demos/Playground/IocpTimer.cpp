#include <barrier>
#include <future>
#include <ios>
#include <random>
#include <iostream>
#include <latch>
#include <print>
#include <semaphore>

#include "Windows.h"

#include <exec/tbb/tbb_thread_pool.hpp>

#include "stdexec/execution.hpp"

int Case1_StopToken() {
    std::ios::sync_with_stdio(false);

    HANDLE hTimer = CreateWaitableTimerEx(nullptr, "Timer", CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (hTimer == nullptr) {
        hTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (hTimer == nullptr) {
            std::cerr << "Failed to create waitable timer: " << GetLastError() << std::endl;
            return 1;
        }
    }

    std::stop_source stop;

    std::stop_token token1 = stop.get_token(), token2 = stop.get_token();

    std::thread t1([hTimer, &token1]() {
        LARGE_INTEGER liDueTime{0ULL};
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Setting timer..." << std::endl;
        SetWaitableTimerEx(hTimer, &liDueTime, 2'000ULL, nullptr, nullptr, nullptr, 0);
    });

    std::thread t2([hTimer, &token2]() {
        while (true) {
            HRESULT ret = WaitForSingleObjectEx(hTimer, INFINITE, TRUE);
            if (ret == WAIT_OBJECT_0) {
                std::cout << "Timer signaled! 1" << std::endl;
                break;
            } else {
                std::cerr << "Wait failed 1: " << GetLastError() << std::endl;
            }
        }

        while (true) {
            HRESULT ret = WaitForSingleObjectEx(hTimer, INFINITE, TRUE);
            if (ret == WAIT_OBJECT_0) {
                std::cout << "Timer signaled! 2" << std::endl;
                break;
            } else {
                std::cerr << "Wait failed 2: " << GetLastError() << std::endl;
            }
        }

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (token2.stop_requested()) {
                std::cout << "Stop requested, exiting thread." << std::endl;
                break;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(6));

    std::cout << "Requesting stop..." << std::endl;
    stop.request_stop();

    std::cout << "Waiting for threads to finish..." << std::endl;

    t1.join();
    t2.join();

    return 0;
}

int Case2_ReverseInform() {
    using namespace stdexec;

    std::cout << "[Main] Starting Case2_ReverseInform" << std::endl;

    inplace_stop_source stop_src;

    HANDLE hTimer = CreateWaitableTimerEx(nullptr, "Timer", CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
    if (hTimer == nullptr) {
        std::cout << "[Main] Failed to create waitable timer" << std::endl;
        return 0;
    }
    std::cout << "[Main] Waitable timer created" << std::endl;

    std::promise<bool> promise;

    std::thread t1([token = stop_src.get_token(), hTimer, &promise]() {
        std::cout << "[T1] Thread started, sleeping 1s" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "[T1] Finished 1s sleep, creating stop_src1" << std::endl;

        std::shared_ptr<inplace_stop_source> stop_src1 = std::make_shared<inplace_stop_source>();
        std::cout << "[T1] stop_src1 created" << std::endl;

        std::thread t11([token1 = stop_src1->get_token(), hTimer]() {
            std::cout << "[T11] Thread started, waiting for stop" << std::endl;
            int loop_count = 0;
            while (!token1.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                loop_count++;
                if (loop_count % 50 == 0) {
                    std::cout << "[T11] Still running... (iteration " << loop_count << ")" << std::endl;
                }
            }
            std::cout << "[T11] Stop requested, exiting wait loop (total iterations: " << loop_count << ")"
                      << std::endl;

            std::cout << "[T11] Sleeping 2s before setting timer" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));

            std::cout << "[T11] Setting waitable timer with 1s interval" << std::endl;
            LARGE_INTEGER li;
            li.QuadPart = 10'000'000ULL;
            if (SetWaitableTimerEx(hTimer, &li, 1'000ULL, nullptr, nullptr, nullptr, 0)) {
                std::cout << "[T11] Timer set successfully" << std::endl;
            } else {
                std::cout << "[T11] Failed to set timer: " << GetLastError() << std::endl;
            }
            std::cout << "[T11] Thread exiting" << std::endl;
        });

        std::cout << "[T1] T11 thread spawned" << std::endl;
        promise.set_value(true);
        std::cout << "[T1] Promise set_value(true) - initialization complete" << std::endl;

        auto f = [stop_src1]() {
            std::cout << "[Callback] Callback invoked! Requesting stop on stop_src1" << std::endl;
            stop_src1->request_stop();
            std::cout << "[Callback] Stop requested" << std::endl;
        };

        std::cout << "[T1] Registering inplace_stop_callback" << std::endl;
        inplace_stop_callback<decltype(f)> callback(token, std::move(f));
        std::cout << "[T1] Callback registered, waiting for parent token and T11 completion..." << std::endl;

        std::cout << "[T1] Joining T11" << std::endl;
        t11.join();
        std::cout << "[T1] T11 joined, stop_src1 will be destroyed" << std::endl;
    });

    std::cout << "[Main] T1 thread spawned" << std::endl;

    std::cout << "[Main] Waiting for T1 initialization (promise.get_future().get())" << std::endl;
    if (promise.get_future().get()) {
        std::cout << "[Main] T1 initialization complete, calling stop_src.request_stop()" << std::endl;
        stop_src.request_stop();
        std::cout << "[Main] stop_src.request_stop() called" << std::endl;
    }

    std::cout << "[Main] Waiting for timer (WaitForSingleObjectEx)" << std::endl;
    if (auto res = WaitForSingleObjectEx(hTimer, INFINITE, FALSE); res == WAIT_OBJECT_0) {
        std::cout << "[Main] Timer signaled! Canceling..." << std::endl;
        CancelWaitableTimer(hTimer);
    } else {
        std::cout << "[Main] WaitForSingleObjectEx returned: " << res << std::endl;
    }

    std::cout << "[Main] Joining T1" << std::endl;
    t1.join();
    std::cout << "[Main] T1 joined, closing timer handle" << std::endl;

    CloseHandle(hTimer);
    std::cout << "[Main] Case2_ReverseInform complete" << std::endl;

    return 0;
}

int Case3_Barrier() {
    auto f = [] { std::cout << "Meow" << std::endl; };
    std::barrier barriers(4, f);

    std::array nums = {1, 2, 3, 4};

    std::array threads{std::thread{[&barriers, &nums] {
                           char name[] = "T1";
                           auto num = nums[0];
                           barriers.arrive_and_wait();
                           nums[3] = num;
                       }},
                       std::thread{[&barriers, &nums] {
                           char name[] = "T2";
                           auto num = nums[1];
                           barriers.arrive_and_wait();
                           nums[2] = num;
                       }},
                       std::thread{[&barriers, &nums] {
                           char name[] = "T3";
                           auto num = nums[2];
                           barriers.arrive_and_wait();
                           nums[1] = num;
                       }},
                       std::thread{[&barriers, &nums] {
                           char name[] = "T4";
                           auto num = nums[3];
                           barriers.arrive_and_wait();
                           nums[0] = num;
                       }}};

    for (auto& t : threads) {
        t.join();
    }

    for (const auto e : nums) {
        std::println("{}", e);
    }

    return 0;
}

int Case4_Latch() {
    std::vector<std::thread> ts;

    constexpr int nt = 10;
    std::latch latch(nt);
    std::latch goLatch(1);

    for (int i = 0; i < nt; ++i) {
        ts.push_back(std::thread([&latch, &goLatch, i] {
            latch.count_down();
            std::cout << "Ready: " + std::to_string(i) << std::endl;
            goLatch.wait();
            std::cout << "Gone: " + std::to_string(i) << std::endl;
        }));
    }

    std::cout << "Main wait ready latch" << std::endl;
    latch.wait();
    std::cout << "Main set allow latch" << std::endl;
    goLatch.count_down();
    std::cout << "Allow latch set" << std::endl;

    for (auto& t : ts) {
        t.join();
    }

    return 0;
}

int Case5_AtomicWait() {
    std::vector<std::thread> ts;

    std::atomic_bool latch;

    constexpr int nt = 10;
    for (int i = 0; i < nt; ++i) {
        ts.push_back(std::thread{[&latch, i] {
            std::cout << "Waiting: " + std::to_string(i) << std::endl;
            if (!latch.load()) {
                latch.wait(true);
            }
            std::cout << "Done: " + std::to_string(i) << std::endl;
        }});
    }

    // std::this_thread::sleep_for(std::chrono::seconds(3)); // wrong without this. still wrong with this.

    latch.store(true, std::memory_order_release);

    for (auto& t : ts) {
        t.join();
    }

    return 0;
}

int Case6_Semaphore() {
    std::counting_semaphore<4> sem(0);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 3000);

    std::stop_source src;

    auto nextInt = [&rd, &gen, &dist] { return dist(gen); };

    std::vector<std::thread> ts;
    ts.reserve(6);

    for (int i = 0; i < 6; ++i) {
        ts.emplace_back(
            [&sem, i, &nextInt](std::stop_token token) {
                while (!token.stop_requested()) {
                    sem.acquire();
                    std::cout << "Thread " + std::to_string(i) + " acquire semaphore"<< std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(nextInt()));
                    sem.release();
                    std::cout << "Thread " + std::to_string(i) + " release semaphore" << std::endl;
                }
            },
            src.get_token());
    }

    std::this_thread::sleep_for(std::chrono::seconds(20));

    for (auto& t : ts) {
        t.join();
    }

    return 0;
}

int main() {
    return Case6_Semaphore();
    return Case5_AtomicWait();
    return Case4_Latch();
    return Case3_Barrier();
    return Case2_ReverseInform();
    return Case1_StopToken();
}
