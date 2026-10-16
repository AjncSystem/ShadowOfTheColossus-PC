// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Common {

// Ordered presenter work, independently of the guest's vblank clock. The frame
// pool bounds guest work; idle redraws are discarded while real work is pending.
class OrderedWorkQueue {
public:
    explicit OrderedWorkQueue(std::function<void()> initialize = {})
        : worker([this, initialize = std::move(initialize)](std::stop_token token) {
            if (initialize) initialize();
            for (;;) {
                std::function<void()> work;
                {
                    std::unique_lock lock(mutex);
                    busy = false;
                    cv.wait(lock, token, [this] { return closed || !queue.empty(); });
                    if (queue.empty() && (closed || token.stop_requested())) return;
                    if (queue.empty()) continue;
                    work = std::move(queue.front());
                    queue.pop_front();
                    busy = true;
                }
                work();
            }
        }) {}

    ~OrderedWorkQueue() { Shutdown(); }
    bool Submit(std::function<void()> work, bool only_if_idle = false) {
        {
            std::scoped_lock lock(mutex);
            if (closed || (only_if_idle && (busy || !queue.empty()))) return false;
            queue.emplace_back(std::move(work));
        }
        cv.notify_one();
        return true;
    }
    void Shutdown() {
        {
            std::scoped_lock lock(mutex);
            closed = true;
        }
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
private:
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::function<void()>> queue;
    bool busy{}, closed{};
    std::jthread worker;
};
} // namespace Common
