// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: worker threads for graphics pipeline creation (vkCreateGraphicsPipelines), adapted from
// gow3_pc's vk_async_compiler.h. Only the driver compile moves off the command processor: shader
// translation, specialization and every guest read stay on it, at the draw.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Vulkan {

/// Bounded task queue. Stop() drains accepted work before Vulkan resources are released.
class AsyncCompiler {
public:
    explicit AsyncCompiler(unsigned count, size_t capacity = 256,
                           std::function<void(unsigned)> initialize = {})
        : capacity{capacity} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, initialize, i] {
                worker_index = i;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty(); });
                        if (tasks.empty()) {
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }
    ~AsyncCompiler() {
        Stop();
    }

    bool Submit(std::function<void()> task) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() >= capacity) {
                return false;
            }
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
        return true;
    }

    /// Index of the calling worker thread (0 outside the workers).
    static unsigned WorkerIndex() {
        return worker_index;
    }

    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

private:
    size_t capacity;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::vector<std::thread> workers;
    static inline thread_local unsigned worker_index = 0;
};

/// One job per pipeline key.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function) {
        auto task = std::make_shared<std::packaged_task<T()>>(std::forward<F>(function));
        auto completion = task->get_future();
        if (!compiler.Submit([task] { (*task)(); })) {
            return false;
        }
        future = std::move(completion);
        return true;
    }
    void Wait() const {
        if (future.valid()) {
            future.wait();
        }
    }
    bool Ready() const {
        return future.valid() &&
               future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    }
    /// The result once ready (rethrows a worker exception).
    T Take() {
        return future.get();
    }

private:
    std::future<T> future;
};

} // namespace Vulkan
