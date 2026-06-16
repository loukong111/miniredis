#pragma once

#include <functional>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace miniredis {

class DynamicThreadPool {
public:
    using Task = std::function<void()>;

    DynamicThreadPool(size_t min_threads = 4, size_t max_threads = 16,
                      size_t idle_timeout_sec = 5,           // 空闲线程退出超时
                      size_t monitor_interval_sec = 2);      // 监控间隔
    ~DynamicThreadPool();

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("submit on stopped DynamicThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
    }

    void stop();
    size_t task_count() const;
    size_t active_threads() const { return active_threads_.load(); }
    size_t idle_threads() const { return idle_threads_.load(); }

private:
    void worker_loop();
    void monitor_loop();            // 监控线程入口
    void try_expand();              // 扩缩容策略调用
    void try_shrink();              // 尝试缩容

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
    std::atomic<size_t> active_threads_;//存活线程
    std::atomic<size_t> idle_threads_;//不处于等待的线程
    size_t min_threads_;
    size_t max_threads_;
    size_t idle_timeout_sec_;
    size_t monitor_interval_sec_;

    std::thread monitor_thread_;    // 监控线程
};

} // namespace miniredis
