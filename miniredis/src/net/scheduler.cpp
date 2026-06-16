#include "miniredis/net/scheduler.hpp"
#include <unistd.h>
#include <sys/eventfd.h>
#include <cassert>
#include <iostream>
#include <errno.h>
#include <cstdint>

namespace miniredis {

void IoAwaitable::await_suspend(std::coroutine_handle<> handle) {
    scheduler_->add_fd(fd_, events_, handle);
}

Scheduler::Scheduler() : epoll_fd_(-1), wake_fd_(-1), running_(false) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        perror("epoll_create1");
        exit(1);
    }
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ == -1) {
        perror("eventfd");
        close(epoll_fd_);
        exit(1);
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = wake_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) == -1) {
        perror("epoll_ctl wake_fd");
        close(wake_fd_);
        close(epoll_fd_);
        exit(1);
    }
}

Scheduler::~Scheduler() {
    stop();
    if (wake_fd_ != -1) close(wake_fd_);
    close(epoll_fd_);
}

void Scheduler::start() {
    running_ = true;
    run_loop();
}

void Scheduler::stop() {
    //只有之前在运行，才需要写 wake_fd_ 唤醒 epoll_wait。如果已经停止了，就没必要重复唤醒。
    bool was_running = running_.exchange(false);
    if (was_running && wake_fd_ != -1) {
        uint64_t value = 1;
        ssize_t ignored = write(wake_fd_, &value, sizeof(value));
        (void)ignored;//消除编译器「变量未使用」警告
    }
}

void Scheduler::schedule_task(Task) {
    // 暂未实现
}

IoAwaitable Scheduler::await_io(int fd, uint32_t events) {
    return IoAwaitable{fd, events, this};
}

void Scheduler::add_fd(int fd, uint32_t events, std::coroutine_handle<> handle) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    fd_to_handle_[fd] = handle;
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    // 尝试先修改（如果已经存在于epoll中），否则添加
    //MOD 失败 + 错误码 ENOENT = fd 不在 epoll 里
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        if (errno == ENOENT) {
            // 不存在，添加
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
                perror("epoll_ctl add");
            }
        } else {
            perror("epoll_ctl mod");
        }
    }
}

void Scheduler::mod_fd(int fd, uint32_t events, std::coroutine_handle<> handle) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    fd_to_handle_[fd] = handle;
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void Scheduler::del_fd(int fd) {
    std::lock_guard<std::mutex> lock(map_mutex_);
    fd_to_handle_.erase(fd);
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void Scheduler::run_loop() {
    const int MAX_EVENTS = 1024;
    struct epoll_event events[MAX_EVENTS];
    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        handle_events(epoll_fd_, events, nfds);
    }
}

void Scheduler::handle_events(int, struct epoll_event* events, int nfds) {
    for (int i = 0; i < nfds; ++i) {
        int fd = events[i].data.fd;
        if (fd == wake_fd_) {
            uint64_t value = 0;
            while (read(wake_fd_, &value, sizeof(value)) > 0) {}
            continue;
        }
        std::coroutine_handle<> handle;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto it = fd_to_handle_.find(fd);
            if (it != fd_to_handle_.end()) {
                handle = it->second;
                // 注意：不要从 map 中删除条目，因为协程恢复后可能立即再次等待同一个 fd
                // 下一次 await_io 会调用 add_fd，它会更新 map 中的 handle 并修改 epoll 事件
            }
        }
        //释放锁后再 resume避免协程恢复后重新进入调度器导致死锁。
        if (handle) {
            handle.resume();
        }
    }
}

}
