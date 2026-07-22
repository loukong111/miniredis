#include "miniredis/core/thread_pool.hpp"
#include <algorithm>
#include <chrono>

namespace miniredis {

DynamicThreadPool::DynamicThreadPool(size_t min_threads, size_t max_threads,
                                     size_t idle_timeout_sec, size_t monitor_interval_sec)
    : stop_(false), active_threads_(0), idle_threads_(0),
      min_threads_(min_threads), max_threads_(max_threads),
      idle_timeout_sec_(idle_timeout_sec),
      monitor_interval_sec_(monitor_interval_sec) {
    if (min_threads_ == 0 || max_threads_ < min_threads_) {
        throw std::invalid_argument("invalid DynamicThreadPool thread limits");
    }
    workers_.reserve(max_threads_);
    finished_workers_.reserve(max_threads_);
    try {
        for (size_t i = 0; i < min_threads_; ++i) {
            spawn_worker();
        }
        monitor_thread_ = std::thread(&DynamicThreadPool::monitor_loop, this);
    } catch (...) {
        stop_.store(true);
        condition_.notify_all();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        throw;
    }
}

DynamicThreadPool::~DynamicThreadPool() {
    stop();
}

void DynamicThreadPool::stop() {
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!tasks_.empty()) tasks_.pop();
    }
    condition_.notify_all();
    monitor_condition_.notify_all();
    if (monitor_thread_.joinable()) monitor_thread_.join();

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

size_t DynamicThreadPool::task_count() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

void DynamicThreadPool::worker_loop() {
    while (!stop_.load()) {
        Task task;
        bool timed_out = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            idle_threads_.fetch_add(1);
            if (tasks_.empty()) {
                const bool awakened = condition_.wait_for(
                    lock, std::chrono::seconds(idle_timeout_sec_),
                    [this] { return stop_.load() || !tasks_.empty(); });
                timed_out = !awakened;
            }
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            idle_threads_.fetch_sub(1);
        }
        if (stop_.load()) break;
        if (task) {
            task();
        } else if (timed_out) {
            size_t current = active_threads_.load();
            while (current > min_threads_) {
                if (active_threads_.compare_exchange_weak(current, current - 1)) {
                    record_finished_worker();
                    return;
                }
            }
        }
    }
    active_threads_.fetch_sub(1);
    record_finished_worker();
}

void DynamicThreadPool::monitor_loop() {
    std::unique_lock<std::mutex> lock(monitor_mutex_);
    while (!stop_.load()) {
        monitor_condition_.wait_for(lock, std::chrono::seconds(monitor_interval_sec_),
                                    [this] { return stop_.load(); });
        if (stop_.load()) break;
        lock.unlock();
        reap_finished_workers();
        try {
            try_expand();
        } catch (...) {
            // Thread creation can fail temporarily; keep the existing workers alive.
        }
        lock.lock();
    }
}

void DynamicThreadPool::try_expand() {
    size_t pending = task_count();
    size_t current = active_threads_;
    // 当待处理任务数大于当前线程数，且未达到最大线程数，且没有空闲线程时，扩容一个线程
    if (pending > current && current < max_threads_ && idle_threads_ == 0) {
        spawn_worker();
    }
}

void DynamicThreadPool::spawn_worker() {
    active_threads_.fetch_add(1);
    try {
        std::thread worker(&DynamicThreadPool::worker_loop, this);
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.push_back(std::move(worker));
    } catch (...) {
        active_threads_.fetch_sub(1);
        throw;
    }
}

void DynamicThreadPool::record_finished_worker() {
    std::lock_guard<std::mutex> lock(finished_workers_mutex_);
    finished_workers_.push_back(std::this_thread::get_id());
}

void DynamicThreadPool::reap_finished_workers() {
    std::lock_guard<std::mutex> finished_lock(finished_workers_mutex_);
    if (finished_workers_.empty()) return;

    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    auto it = workers_.begin();
    while (it != workers_.end()) {
        if (std::find(finished_workers_.begin(), finished_workers_.end(), it->get_id()) !=
            finished_workers_.end()) {
            if (it->joinable()) it->join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
    finished_workers_.clear();
}

} // namespace miniredis
