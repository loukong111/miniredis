#include "miniredis/core/thread_pool.hpp"
#include <chrono>
#include <iostream>

namespace miniredis {

DynamicThreadPool::DynamicThreadPool(size_t min_threads, size_t max_threads,
                                     size_t idle_timeout_sec, size_t monitor_interval_sec)
    : stop_(false), active_threads_(0), idle_threads_(0),
      min_threads_(min_threads), max_threads_(max_threads),
      idle_timeout_sec_(idle_timeout_sec),
      monitor_interval_sec_(monitor_interval_sec) {
    // 启动最小数量的工作线程
    for (size_t i = 0; i < min_threads_; ++i) {
        workers_.emplace_back(&DynamicThreadPool::worker_loop, this);
        active_threads_++;
    }
    // 启动监控线程
    monitor_thread_ = std::thread(&DynamicThreadPool::monitor_loop, this);
}

DynamicThreadPool::~DynamicThreadPool() {
    stop();
}

void DynamicThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

size_t DynamicThreadPool::task_count() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

void DynamicThreadPool::worker_loop() {
    while (!stop_) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            idle_threads_++;
            if (tasks_.empty()) {
                condition_.wait_for(lock, std::chrono::seconds(idle_timeout_sec_),
                                    [this] { return stop_ || !tasks_.empty(); });
            }
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            idle_threads_--;
        }
        if (task) {
            task();
        } else {
            // 超时无任务且当前线程数大于最小线程数，退出此线程
            if (!stop_ && active_threads_ > min_threads_) {
                // 通过 break 退出循环，线程结束
                break;
            }
        }
    }
    active_threads_--;
}

void DynamicThreadPool::monitor_loop() {
    while (!stop_) {
        std::this_thread::sleep_for(std::chrono::seconds(monitor_interval_sec_));
        if (stop_) break;
        try_expand();
        try_shrink();
    }
}

void DynamicThreadPool::try_expand() {
    size_t pending = task_count();
    size_t current = active_threads_;
    // 当待处理任务数大于当前线程数，且未达到最大线程数，且没有空闲线程时，扩容一个线程
    if (pending > current && current < max_threads_ && idle_threads_ == 0) {
        workers_.emplace_back(&DynamicThreadPool::worker_loop, this);
        active_threads_++;
    }
}

void DynamicThreadPool::try_shrink() {
    // 缩容条件：当前工作线程数 > 最小线程数，任务队列为空，空闲线程数 > 0（说明有空闲线程可以回收）
    size_t current = active_threads_;
    if (current <= min_threads_) return;
    // 检查任务队列是否为空（加锁）
    bool queue_empty = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_empty = tasks_.empty();
    }
    if (!queue_empty) return;
    if (idle_threads_ > 0) {
        // 唤醒一个工作线程，让它检查可退出条件
        condition_.notify_one();
    }
}

} // namespace miniredis