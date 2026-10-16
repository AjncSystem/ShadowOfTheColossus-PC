// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include "common/ordered_work_queue.h"
#include "common/thread.h"

int main() {
    using namespace std::chrono;
    std::promise<void> entered, release;
    const auto gate=release.get_future().share();
    Common::OrderedWorkQueue present;
    std::vector<int> order;
    assert(present.Submit([&] { entered.set_value(); gate.wait(); order.push_back(0); }));
    entered.get_future().wait();
    assert(!present.Submit([&] { order.push_back(99); },true));
    assert(present.Submit([&] { order.push_back(1); }));
    assert(present.Submit([&] { order.push_back(2); }));
    // An artificial blocked presenter cannot stall the independent vblank producer.
    std::atomic<unsigned> ticks{};
    std::jthread vblank([&](std::stop_token token) {
        while (!token.stop_requested()) {
            ++ticks;
            Common::PreciseSleepUntil(steady_clock::now()+milliseconds(4));
        }
    });
    std::this_thread::sleep_for(milliseconds(80));
    assert(ticks.load()>=5);
    release.set_value();
    present.Shutdown();
    vblank.request_stop();
    vblank.join();
    assert((order==std::vector<int>{0,1,2}));
    assert(!present.Submit([] {}));
    Common::OrderedWorkQueue concurrent;
    std::array<std::vector<int>,4> results;
    std::vector<std::jthread> producers;
    for (unsigned p=0;p<4;++p) producers.emplace_back([&,p] {
        for (int i=0;i<100;++i) assert(concurrent.Submit([&,p,i] { results[p].push_back(i); }));
    });
    for (auto& thread:producers) thread.join();
    concurrent.Shutdown();
    for (const auto& items:results) {
        assert(items.size()==100);
        for (int i=0;i<100;++i) assert(items[i]==i);
    }
    // A late tick has bounded debt and cannot cause dozens of immediate catch-up vblanks.
    Common::AccurateTimer timer{milliseconds(4)};
    timer.Start();
    std::this_thread::sleep_for(milliseconds(80));
    timer.End();
    assert(timer.GetTotalWait()>=-milliseconds(4));
    const auto start=steady_clock::now();
    Common::PreciseSleepUntil(start+milliseconds(5));
    assert(steady_clock::now()-start>=milliseconds(5));
    std::cout << "Ordered present, redraw coalescing, independent clock, drain, concurrent producers and timer passed\n";
}
