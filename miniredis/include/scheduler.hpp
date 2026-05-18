#pragma once

#include <coroutine>
#include <functional>
#include <memory>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace miniredis {

class Scheduler;

struct IoAwaitable {
    int fd_;
    uint32_t events_;
    Scheduler* scheduler_;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);
    void await_resume() const noexcept {}
};

struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> handle;
    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if (handle) handle.destroy(); }
    Task(Task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    void resume() { if (handle) handle.resume(); }
    bool done() const { return handle.done(); }   // 添加这一行
};

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void start();
    void stop();

    void schedule_task(Task task);   // 暂不实现，可留空或后续扩展

    IoAwaitable await_io(int fd, uint32_t events);

    void add_fd(int fd, uint32_t events, std::coroutine_handle<> handle);
    void mod_fd(int fd, uint32_t events, std::coroutine_handle<> handle);
    void del_fd(int fd);

private:
    void run_loop();
    void handle_events(int epollfd, struct epoll_event* events, int nfds);

    int epoll_fd_;
    std::atomic<bool> running_;
    std::unordered_map<int, std::coroutine_handle<>> fd_to_handle_;
    std::mutex map_mutex_;
};

}