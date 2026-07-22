#include "miniredis/server/replication_dispatcher.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/metrics/stats.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace miniredis {
namespace {

constexpr int kConnectTimeoutMs = 1500;
constexpr int kIoTimeoutSec = 2;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

bool splitNodeAddr(const std::string& node, std::string& host, int& port) {
    size_t pos = node.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= node.size()) return false;
    host = node.substr(0, pos);
    const char* begin = node.data() + pos + 1;
    const char* end = node.data() + node.size();
    auto [ptr, ec] = std::from_chars(begin, end, port);
    return ec == std::errc() && ptr == end && port > 0 && port <= 65535;
}

std::string encodeRespCommand(const std::vector<std::string>& command) {
    std::string out = "*" + std::to_string(command.size()) + "\r\n";
    for (const auto& part : command) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out += part;
        out += "\r\n";
    }
    return out;
}

bool writeAll(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool readLine(int fd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (line.size() <= 4096) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n == 1) {
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                return true;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return false;
}

bool parseIntegerResponse(const std::string& response, uint64_t& value) {
    if (response.size() < 4 || response.front() != ':' ||
        response[response.size() - 2] != '\r' || response.back() != '\n') {
        return false;
    }
    const char* begin = response.data() + 1;
    const char* end = response.data() + response.size() - 2;
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc() && ptr == end;
}

void updateMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {}
}

} // namespace

struct ReplicationDispatcher::Worker {
    explicit Worker(std::string replica) : node(std::move(replica)) {}

    std::string node;
    std::atomic<int> fd{-1};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> acknowledged_offset{0};
    std::atomic<uint64_t> target_offset{0};
    std::atomic<uint64_t> successful_connections{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> backlog_misses{0};
    std::thread thread;
    std::mutex wait_mutex;
    std::condition_variable wakeup;
    mutable std::mutex error_mutex;
    std::string last_error;
};

ReplicationDispatcher::ReplicationDispatcher(ReplicationBacklog& backlog,
                                             std::vector<std::string> replicas,
                                             std::string password,
                                             std::chrono::milliseconds reconnect_delay)
    : backlog_(backlog),
      password_(std::move(password)),
      reconnect_delay_(std::max(reconnect_delay, std::chrono::milliseconds(50))) {
    workers_.reserve(replicas.size());
    for (auto& replica : replicas) {
        if (!replica.empty()) workers_.push_back(std::make_unique<Worker>(std::move(replica)));
    }
}

ReplicationDispatcher::~ReplicationDispatcher() {
    stop();
}

void ReplicationDispatcher::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    const uint64_t offset = backlog_.currentOffset();
    for (auto& worker : workers_) {
        worker->target_offset.store(offset, std::memory_order_relaxed);
        worker->thread = std::thread([this, worker = worker.get()]() { workerLoop(*worker); });
    }
    publishStats();
}

void ReplicationDispatcher::stop() {
    if (!running_.exchange(false)) return;

    for (auto& worker : workers_) {
        worker->wakeup.notify_all();
        int fd = worker->fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
    }
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
        worker->connected.store(false, std::memory_order_relaxed);
    }
    publishStats();
}

void ReplicationDispatcher::notify(uint64_t offset) {
    if (offset == 0) return;
    for (auto& worker : workers_) {
        updateMax(worker->target_offset, offset);
        worker->wakeup.notify_one();
    }
    publishStats();
}

std::vector<ReplicaDispatchStatus> ReplicationDispatcher::status() const {
    std::vector<ReplicaDispatchStatus> result;
    result.reserve(workers_.size());
    for (const auto& worker : workers_) {
        ReplicaDispatchStatus item;
        item.node = worker->node;
        item.connected = worker->connected.load(std::memory_order_relaxed);
        item.acknowledged_offset = worker->acknowledged_offset.load(std::memory_order_relaxed);
        item.target_offset = worker->target_offset.load(std::memory_order_relaxed);
        const uint64_t connections =
            worker->successful_connections.load(std::memory_order_relaxed);
        item.reconnects = connections > 0 ? connections - 1 : 0;
        item.errors = worker->errors.load(std::memory_order_relaxed);
        item.backlog_misses = worker->backlog_misses.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(worker->error_mutex);
            item.last_error = worker->last_error;
        }
        result.push_back(std::move(item));
    }
    return result;
}

void ReplicationDispatcher::workerLoop(Worker& worker) {
    while (running_.load(std::memory_order_acquire)) {
        if (worker.fd.load(std::memory_order_relaxed) < 0 && !connectReplica(worker)) {
            std::unique_lock<std::mutex> lock(worker.wait_mutex);
            worker.wakeup.wait_for(lock, reconnect_delay_,
                                   [this]() { return !running_.load(std::memory_order_acquire); });
            continue;
        }

        uint64_t remote_offset = 0;
        if (!queryReplicaOffset(worker, remote_offset)) {
            recordFailure(worker, "replica ACK failed");
            disconnect(worker);
            continue;
        }
        worker.acknowledged_offset.store(remote_offset, std::memory_order_relaxed);
        worker.connected.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(worker.error_mutex);
            worker.last_error.clear();
        }
        publishStats();

        const uint64_t target = worker.target_offset.load(std::memory_order_acquire);
        if (remote_offset >= target) {
            std::unique_lock<std::mutex> lock(worker.wait_mutex);
            worker.wakeup.wait_for(lock, kHeartbeatInterval, [this, &worker, target]() {
                return !running_.load(std::memory_order_acquire) ||
                       worker.target_offset.load(std::memory_order_acquire) > target;
            });
            continue;
        }

        std::vector<ReplicationBacklogEntry> entries;
        if (!backlog_.entriesAfter(remote_offset, entries)) {
            recordFailure(worker, "replica offset is outside the replication backlog", true);
            disconnect(worker);
            std::unique_lock<std::mutex> lock(worker.wait_mutex);
            worker.wakeup.wait_for(lock, std::max(reconnect_delay_, std::chrono::milliseconds(1000)),
                                   [this]() { return !running_.load(std::memory_order_acquire); });
            continue;
        }

        bool sent = true;
        for (const auto& entry : entries) {
            if (!running_.load(std::memory_order_acquire)) break;
            if (!sendEntry(worker, entry)) {
                recordFailure(worker, "failed to send replication entry at offset " +
                                      std::to_string(entry.offset));
                disconnect(worker);
                sent = false;
                break;
            }
            worker.acknowledged_offset.store(entry.offset, std::memory_order_release);
            updateMax(worker.target_offset, entry.offset);
        }
        if (sent) publishStats();
    }

    disconnect(worker);
}

bool ReplicationDispatcher::connectReplica(Worker& worker) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(worker.node, host, port)) {
        recordFailure(worker, "invalid replica address");
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        recordFailure(worker, std::string("socket failed: ") + std::strerror(errno));
        return false;
    }
    worker.fd.store(fd, std::memory_order_release);

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        recordFailure(worker, std::string("fcntl failed: ") + std::strerror(errno));
        disconnect(worker);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        recordFailure(worker, "replica address must be an IPv4 address");
        disconnect(worker);
        return false;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        recordFailure(worker, std::string("connect failed: ") + std::strerror(errno));
        disconnect(worker);
        return false;
    }
    if (rc < 0) {
        pollfd event{fd, POLLOUT, 0};
        do {
            rc = ::poll(&event, 1, kConnectTimeoutMs);
        } while (rc < 0 && errno == EINTR);
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (rc <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0 ||
            socket_error != 0) {
            if (socket_error != 0) errno = socket_error;
            recordFailure(worker, std::string("connect failed: ") + std::strerror(errno));
            disconnect(worker);
            return false;
        }
    }

    if (::fcntl(fd, F_SETFL, flags) < 0) {
        recordFailure(worker, std::string("fcntl restore failed: ") + std::strerror(errno));
        disconnect(worker);
        return false;
    }
    timeval timeout{};
    timeout.tv_sec = kIoTimeoutSec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));

    std::string response;
    if (!password_.empty() &&
        (!writeAll(fd, encodeRespCommand({"AUTH", password_})) ||
         !readLine(fd, response) || response.rfind("+OK", 0) != 0)) {
        recordFailure(worker, "replica authentication failed");
        disconnect(worker);
        return false;
    }

    worker.successful_connections.fetch_add(1, std::memory_order_relaxed);
    MINIREDIS_LOG_INFO("replication") << "connected async replication stream to " << worker.node;
    return true;
}

bool ReplicationDispatcher::queryReplicaOffset(Worker& worker, uint64_t& offset) {
    const int fd = worker.fd.load(std::memory_order_acquire);
    if (fd < 0 || !writeAll(fd, encodeRespCommand({"REPLACK"}))) return false;
    std::string response;
    return readLine(fd, response) && parseIntegerResponse(response, offset);
}

bool ReplicationDispatcher::sendEntry(Worker& worker,
                                      const ReplicationBacklogEntry& entry) {
    std::vector<std::string> command;
    command.reserve(entry.command.size() + 2);
    command.push_back("REPLAPPLY");
    command.push_back(std::to_string(entry.offset));
    command.insert(command.end(), entry.command.begin(), entry.command.end());

    const int fd = worker.fd.load(std::memory_order_acquire);
    if (fd < 0 || !writeAll(fd, encodeRespCommand(command))) return false;
    std::string response;
    return readLine(fd, response) && response.rfind("+OK", 0) == 0;
}

void ReplicationDispatcher::disconnect(Worker& worker) {
    const int fd = worker.fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    worker.connected.store(false, std::memory_order_relaxed);
    publishStats();
}

void ReplicationDispatcher::recordFailure(Worker& worker, const std::string& error,
                                          bool backlog_miss) {
    worker.errors.fetch_add(1, std::memory_order_relaxed);
    if (backlog_miss) worker.backlog_misses.fetch_add(1, std::memory_order_relaxed);

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(worker.error_mutex);
        changed = worker.last_error != error;
        worker.last_error = error;
    }
    if (changed) {
        MINIREDIS_LOG_WARN("replication") << worker.node << ": " << error;
    }
    publishStats();
}

void ReplicationDispatcher::publishStats() const {
    size_t connected = 0;
    uint64_t minimum_ack = std::numeric_limits<uint64_t>::max();
    uint64_t reconnects = 0;
    uint64_t errors = 0;
    uint64_t backlog_misses = 0;
    const uint64_t master_offset = backlog_.currentOffset();
    uint64_t pending = 0;

    for (const auto& worker : workers_) {
        if (worker->connected.load(std::memory_order_relaxed)) ++connected;
        const uint64_t ack = worker->acknowledged_offset.load(std::memory_order_relaxed);
        minimum_ack = std::min(minimum_ack, ack);
        if (master_offset > ack) pending += master_offset - ack;
        const uint64_t connections =
            worker->successful_connections.load(std::memory_order_relaxed);
        if (connections > 0) reconnects += connections - 1;
        errors += worker->errors.load(std::memory_order_relaxed);
        backlog_misses += worker->backlog_misses.load(std::memory_order_relaxed);
    }
    if (workers_.empty()) minimum_ack = 0;

    Stats::instance().setReplicationState(workers_.size(), connected, master_offset,
                                          minimum_ack, pending, reconnects,
                                          errors, backlog_misses);
}

} // namespace miniredis
