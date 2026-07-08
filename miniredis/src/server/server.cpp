#include "miniredis/server/server.hpp"
#include "miniredis/cluster/cluster_config_store.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/metrics/http_stats.hpp"
#include "miniredis/metrics/stats.hpp"
#include "miniredis/persistence/persistence_manager.hpp"
#include "miniredis/server/command_handler.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace miniredis {
namespace {

std::atomic<bool>* g_running_signal_target = nullptr;

void signalHandler(int) {
    if (g_running_signal_target) {
        g_running_signal_target->store(false);
    }
}

bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool splitNodeAddr(const std::string& node, std::string& host, int& port) {
    size_t pos = node.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= node.size()) return false;
    host = node.substr(0, pos);
    try {
        port = std::stoi(node.substr(pos + 1));
    } catch (...) {
        return false;
    }
    return port > 0 && port <= 65535;
}

std::string encodeRespCommand(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& part : parts) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out += part + "\r\n";
    }
    return out;
}

bool writeAllBlocking(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool readRespLine(int fd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (line.size() < 1024) {
        ssize_t n = read(fd, &ch, 1);
        if (n > 0) {
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line[line.size() - 1] == '\n') {
                return true;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return false;
}

bool readRespValueBlocking(int fd, RespValue& value) {
    constexpr size_t kMaxReplicationSnapshotBytes = 256ULL * 1024ULL * 1024ULL;
    RespDecoder decoder;
    char buffer[4096];
    size_t total_read = 0;
    while (total_read < kMaxReplicationSnapshotBytes) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            total_read += static_cast<size_t>(n);
            decoder.feed(std::string_view(buffer, static_cast<size_t>(n)));
            auto parsed = decoder.parse();
            if (parsed) {
                value = std::move(*parsed);
                return true;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return false;
}

bool sendHeartbeatCommand(int fd, const std::vector<std::string>& command, const std::string& expected) {
    if (!writeAllBlocking(fd, encodeRespCommand(command))) return false;
    std::string line;
    if (!readRespLine(fd, line)) return false;
    return line.rfind(expected, 0) == 0;
}

bool parseClusterSlotMapResponse(const RespValue& response, ClusterSlotMapSnapshot& snapshot) {
    if (response.type != RespType::ARRAY) return false;

    snapshot = ClusterSlotMapSnapshot{};
    snapshot.slot_owner.assign(kRedisClusterSlots, "");
    snapshot.slot_meta.assign(kRedisClusterSlots, ClusterSlotMeta{});
    std::unordered_set<std::string> known_nodes;
    bool saw_epoch = false;
    bool saw_nodes = false;
    bool saw_slots = false;

    for (const auto& section : response.array) {
        if (section.type != RespType::ARRAY || section.array.size() != 2 ||
            section.array[0].type != RespType::BULK_STRING) {
            return false;
        }
        std::string tag = section.array[0].str;
        for (char& ch : tag) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }

        const RespValue& payload = section.array[1];
        if (tag == "EPOCH") {
            if (payload.type != RespType::INTEGER || payload.integer < 0) return false;
            snapshot.epoch = static_cast<uint64_t>(payload.integer);
            saw_epoch = true;
        } else if (tag == "NODES") {
            if (payload.type != RespType::ARRAY) return false;
            for (const auto& item : payload.array) {
                if (item.type != RespType::ARRAY || item.array.size() != 2 ||
                    item.array[0].type != RespType::BULK_STRING ||
                    item.array[1].type != RespType::BULK_STRING) {
                    return false;
                }
                const std::string& node = item.array[0].str;
                ClusterNodeState state = ClusterNodeState::Healthy;
                if (node.empty() || !parseClusterNodeState(item.array[1].str, state) ||
                    !known_nodes.insert(node).second) {
                    return false;
                }
                snapshot.nodes.push_back(node);
                snapshot.node_states[node] = state;
            }
            saw_nodes = true;
        } else if (tag == "SLOTS") {
            if (payload.type != RespType::ARRAY) return false;
            for (const auto& item : payload.array) {
                if (item.type != RespType::ARRAY || item.array.size() != 3 ||
                    item.array[0].type != RespType::INTEGER ||
                    item.array[1].type != RespType::INTEGER ||
                    item.array[2].type != RespType::BULK_STRING) {
                    return false;
                }
                long long start = item.array[0].integer;
                long long end = item.array[1].integer;
                const std::string& owner = item.array[2].str;
                if (start < 0 || end < start || end >= kRedisClusterSlots ||
                    known_nodes.find(owner) == known_nodes.end()) {
                    return false;
                }
                for (long long slot = start; slot <= end; ++slot) {
                    snapshot.slot_owner[static_cast<size_t>(slot)] = owner;
                }
            }
            saw_slots = true;
        } else if (tag == "SLOTSTATES") {
            if (payload.type != RespType::ARRAY) return false;
            for (const auto& item : payload.array) {
                if (item.type != RespType::ARRAY || item.array.size() != 3 ||
                    item.array[0].type != RespType::INTEGER ||
                    item.array[1].type != RespType::BULK_STRING ||
                    item.array[2].type != RespType::BULK_STRING) {
                    return false;
                }
                long long slot = item.array[0].integer;
                ClusterSlotState state = ClusterSlotState::Stable;
                const std::string& peer = item.array[2].str;
                if (slot < 0 || slot >= kRedisClusterSlots ||
                    !parseClusterSlotState(item.array[1].str, state) ||
                    state == ClusterSlotState::Stable ||
                    known_nodes.find(peer) == known_nodes.end()) {
                    return false;
                }
                snapshot.slot_meta[static_cast<size_t>(slot)] = ClusterSlotMeta{state, peer};
            }
        } else {
            return false;
        }
    }

    return saw_epoch && saw_nodes && saw_slots && !snapshot.nodes.empty();
}

bool fetchClusterSlotMap(const std::string& node,
                         const std::string& password,
                         ClusterSlotMapSnapshot& snapshot) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(node, host, port)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    bool ok = true;
    std::string line;
    if (!password.empty()) {
        ok = writeAllBlocking(fd, encodeRespCommand({"AUTH", password})) &&
             readRespLine(fd, line) && line.rfind("+OK", 0) == 0;
    }

    RespValue response;
    if (ok) {
        ok = writeAllBlocking(fd, encodeRespCommand({"CLUSTER", "SLOTMAP"})) &&
             readRespValueBlocking(fd, response);
    }
    close(fd);
    return ok && parseClusterSlotMapResponse(response, snapshot);
}

std::vector<std::string> slowLogCommandParts(const RespValue& cmd) {
    std::vector<std::string> parts;
    if (cmd.type != RespType::ARRAY) return parts;
    parts.reserve(cmd.array.size());
    for (const auto& arg : cmd.array) {
        if (arg.type == RespType::BULK_STRING || arg.type == RespType::SIMPLE_STRING) {
            std::string value = arg.str;
            if (value.size() > 128) {
                value.resize(128);
                value += "...";
            }
            parts.push_back(std::move(value));
        } else if (arg.type == RespType::INTEGER) {
            parts.push_back(std::to_string(arg.integer));
        } else {
            parts.push_back("<non-string>");
        }
    }
    if (!parts.empty()) {
        std::string name = parts[0];
        for (char& ch : name) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (name == "AUTH" && parts.size() > 1) {
            for (size_t i = 1; i < parts.size(); ++i) {
                parts[i] = "*****";
            }
        }
    }
    return parts;
}

bool pingClusterNode(const std::string& node, const std::string& password) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(node, host, port)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    timeval timeout{};
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    bool ok = true;
    if (!password.empty()) {
        ok = sendHeartbeatCommand(fd, {"AUTH", password}, "+OK");
    }
    if (ok) {
        ok = sendHeartbeatCommand(fd, {"PING"}, "+PONG");
    }
    close(fd);
    return ok;
}

std::vector<std::string> parseClusterNodes(const std::string& nodes_str) {
    std::vector<std::string> nodes;
    size_t start = 0;
    size_t end = 0;
    while ((end = nodes_str.find(',', start)) != std::string::npos) {
        std::string node = nodes_str.substr(start, end - start);
        if (!node.empty()) nodes.push_back(node);
        start = end + 1;
    }
    std::string last = nodes_str.substr(start);
    if (!last.empty()) nodes.push_back(last);
    return nodes;
}

EvictionPolicy parseEvictionPolicy(const std::string& policy) {
    if (policy == "lru") return EvictionPolicy::Lru;
    return EvictionPolicy::NoEviction;
}

class ClientConnection {
public:
    ClientConnection(Scheduler& scheduler, int fd) : scheduler_(scheduler), fd_(fd) {}
    ~ClientConnection() { closeNow(); }

    int fd() const { return fd_; }

    void closeNow() {
        if (fd_ < 0) return;
        scheduler_.del_fd(fd_);
        close(fd_);
        fd_ = -1;
        Stats::instance().recordConnectionClose();
    }

private:
    Scheduler& scheduler_;
    int fd_;
};

Task handleClient(Scheduler& scheduler, CommandHandler& command_handler,
                  const AppConfig& config, int client_fd) {
    ClientConnection client(scheduler, client_fd);
    RespDecoder decoder;
    char buffer[4096];
    std::string outbuf;
    bool writing = false;
    bool close_after_write = false;
    CommandSession session;
    session.authenticated = config.requirepass.empty() && config.acl_users.empty();
    session.role = AclRole::Admin;

    while (true) {
        if (!writing) {
            co_await scheduler.await_io(client.fd(), EPOLLIN);
        }
        if (!writing) {
            ssize_t n = read(client.fd(), buffer, sizeof(buffer));
            if (n == 0) {
                client.closeNow();
                break;
            }
            if (n < 0) {
                if (errno == EINTR || wouldBlock()) {
                    continue;
                }
                MINIREDIS_LOG_WARN("server") << "read error: " << std::strerror(errno);
                client.closeNow();
                break;
            }

            decoder.feed(std::string_view(buffer, static_cast<size_t>(n)));
            if (decoder.bufferedSize() > config.max_request_bytes) {
                outbuf += RespWriter::error("request too large");
                close_after_write = true;
                writing = true;
            }

            size_t parsed_commands = 0;
            while (!close_after_write) {
                auto opt_cmd = decoder.parse();
                if (!opt_cmd) break;
                ++parsed_commands;
                if (parsed_commands > config.max_pipeline_commands) {
                    outbuf = RespWriter::error("pipeline command limit exceeded");
                    close_after_write = true;
                    writing = true;
                    break;
                }
                auto start = std::chrono::steady_clock::now();
                std::string response = command_handler.handle(*opt_cmd, session);
                if (outbuf.size() + response.size() > config.client_output_buffer_limit) {
                    outbuf = RespWriter::error("client output buffer limit exceeded");
                    close_after_write = true;
                    writing = true;
                    break;
                }
                outbuf += std::move(response);
                auto end = std::chrono::steady_clock::now();
                auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                Stats::instance().recordCommandLatency(static_cast<size_t>(latency_us),
                                                       slowLogCommandParts(*opt_cmd));
            }
        }

        if (!outbuf.empty()) {
            ssize_t n = write(client.fd(), outbuf.data(), outbuf.size());
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    writing = true;
                    co_await scheduler.await_io(client.fd(), EPOLLOUT);
                    continue;
                }
                MINIREDIS_LOG_WARN("server") << "write error: " << std::strerror(errno);
                client.closeNow();
                break;
            }
            if (n == 0) {
                writing = true;
                co_await scheduler.await_io(client.fd(), EPOLLOUT);
                continue;
            }
            if (n == static_cast<ssize_t>(outbuf.size())) {
                outbuf.clear();
                writing = false;
                if (close_after_write) {
                    client.closeNow();
                    break;
                }
            } else {
                outbuf.erase(0, static_cast<size_t>(n));
                writing = true;
                co_await scheduler.await_io(client.fd(), EPOLLOUT);
            }
        } else {
            writing = false;
        }
    }
    co_return;
}

Task acceptLoop(Scheduler& accept_scheduler,
                std::vector<std::unique_ptr<Scheduler>>& io_schedulers,
                std::atomic<size_t>& next_io_scheduler,
                CommandHandler& command_handler,
                const AppConfig& config, std::atomic<bool>& running, int listen_fd) {
    while (running.load()) {
        co_await accept_scheduler.await_io(listen_fd, EPOLLIN);
        while (true) {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    MINIREDIS_LOG_WARN("server") << "accept failed: " << std::strerror(errno);
                }
                break;
            }
            if (!setNonBlocking(client_fd)) {
                MINIREDIS_LOG_WARN("server") << "client fcntl failed: " << std::strerror(errno);
                Stats::instance().recordRejectedConnection();
                close(client_fd);
                continue;
            }
            if (Stats::instance().connectedClients() >= config.max_clients) {
                Stats::instance().recordRejectedConnection();
                const char* error = "-ERR max number of clients reached\r\n";
                ssize_t ignored = write(client_fd, error, std::strlen(error));
                (void)ignored;
                close(client_fd);
                continue;
            }
            Stats::instance().recordConnectionOpen();
            size_t idx = next_io_scheduler.fetch_add(1, std::memory_order_relaxed) %
                         io_schedulers.size();
            Scheduler& worker = *io_schedulers[idx];
            worker.schedule_task(handleClient(worker, command_handler, config, client_fd));
        }
    }
}

} // namespace

MiniRedisServer::MiniRedisServer(AppConfig config)
    : config_(std::move(config)),
      memory_pool_(1024),
      thread_pool_(4, 16, 5),
      cache_(memory_pool_, config_.cache_shards),
      replication_backlog_(10000),
      replication_offset_(0),
      next_io_scheduler_(0),
      running_(true),
      stats_running_(false),
      listen_fd_(-1) {}

MiniRedisServer::~MiniRedisServer() {
    stop();
}

bool MiniRedisServer::configureCluster() {
    if (config_.cluster_mode) {
        if (config_.node_addr.empty()) {
            MINIREDIS_LOG_ERROR("cluster") << "cluster mode requires --node-addr";
            return false;
        }
        current_node_ = config_.node_addr;
        if (!parseNodePort(current_node_, config_.port)) {
            MINIREDIS_LOG_ERROR("cluster") << "invalid --node-addr, expected ip:port";
            return false;
        }

        slot_map_ = std::make_unique<ClusterSlotMap>();
        bool loaded = false;
        if (!config_.cluster_config_file.empty()) {
            loaded = loadClusterConfig(config_.cluster_config_file, *slot_map_);
        }

        if (!loaded) {
            auto nodes = parseClusterNodes(config_.nodes_str);
            if (nodes.empty()) {
                MINIREDIS_LOG_ERROR("cluster")
                    << "cluster mode requires --nodes when cluster config is unavailable";
                return false;
            }
            slot_map_->Rebuild(nodes);
            saveClusterConfigIfNeeded();
        }

        auto nodes = slot_map_->GetAllNodes();
        if (std::find(nodes.begin(), nodes.end(), current_node_) == nodes.end()) {
            MINIREDIS_LOG_ERROR("cluster")
                << "current node is not present in cluster config/nodes: " << current_node_;
            return false;
        }
    } else {
        std::ostringstream node;
        node << config_.bind_addr << ":" << config_.port;
        current_node_ = node.str();
    }

    if (config_.snapshot_file.empty()) {
        config_.snapshot_file = "snapshot_" + std::to_string(config_.port) + ".dat";
    }
    Stats::instance().setNodeAddr(current_node_);
    return true;
}

void MiniRedisServer::saveClusterConfigIfNeeded() const {
    if (!config_.cluster_mode || !slot_map_ || config_.cluster_config_file.empty()) return;
    if (!saveClusterConfig(config_.cluster_config_file, *slot_map_)) {
        MINIREDIS_LOG_WARN("cluster") << "failed to persist cluster config: "
                                      << config_.cluster_config_file;
    }
}

int MiniRedisServer::bindListen() const {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        MINIREDIS_LOG_ERROR("server") << "socket failed: " << std::strerror(errno);
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.bind_addr.c_str(), &addr.sin_addr) != 1) {
        MINIREDIS_LOG_ERROR("server") << "invalid bind address: " << config_.bind_addr;
        close(listen_fd);
        return -1;
    }
    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        MINIREDIS_LOG_ERROR("server") << "bind failed: " << std::strerror(errno);
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 128) < 0) {
        MINIREDIS_LOG_ERROR("server") << "listen failed: " << std::strerror(errno);
        close(listen_fd);
        return -1;
    }
    if (!setNonBlocking(listen_fd)) {
        MINIREDIS_LOG_ERROR("server") << "fcntl failed: " << std::strerror(errno);
        close(listen_fd);
        return -1;
    }

    MINIREDIS_LOG_INFO("server") << "listening on " << config_.bind_addr << ":" << config_.port;
    return listen_fd;
}

void MiniRedisServer::startStatsServer() {
    stats_running_ = true;
    stats_thread_ = std::thread([this]() {
        start_stats_http_server(config_.stats_bind_addr, config_.stats_port, stats_running_);
    });
}

void MiniRedisServer::startClusterDiscovery() {
    if (!config_.cluster_mode || !slot_map_) return;

#ifdef HAVE_MYSQL
    if (config_.enable_node_discovery) {
        try {
            mysql_ = std::make_unique<MySQLClient>(config_.mysql_host, config_.mysql_user,
                                                   config_.mysql_pass, config_.mysql_db,
                                                   static_cast<unsigned int>(config_.mysql_port));
        } catch (const std::exception& e) {
            MINIREDIS_LOG_WARN("cluster") << "mysql cluster discovery disabled: " << e.what();
        }

        if (mysql_ && !mysql_->registerNode(current_node_, 30)) {
            MINIREDIS_LOG_WARN("cluster") << "failed to register node in cluster table";
        }
    }
#else
    if (config_.enable_node_discovery) {
        MINIREDIS_LOG_WARN("cluster") << "mysql support is not available; dynamic node discovery disabled";
    }
#endif

    cluster_refresh_thread_ = std::thread([this]() {
        std::unordered_map<std::string, int> failed_heartbeats;
        int discovery_elapsed_sec = 0;

        while (running_.load() && config_.cluster_mode) {
            for (int i = 0; i < config_.cluster_heartbeat_interval_sec && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!running_.load()) break;

#ifdef HAVE_MYSQL
            discovery_elapsed_sec += config_.cluster_heartbeat_interval_sec;
            if (mysql_ && discovery_elapsed_sec >= 10) {
                discovery_elapsed_sec = 0;
                auto nodes = mysql_->getActiveNodes(30);
                if (!nodes.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(slot_map_mutex_);
                        slot_map_->Rebuild(nodes);
                    }
                    saveClusterConfigIfNeeded();
                }
            }
#endif

            std::vector<std::string> nodes;
            {
                std::lock_guard<std::mutex> lock(slot_map_mutex_);
                nodes = slot_map_->GetAllNodes();
            }

            for (const auto& node : nodes) {
                if (node == current_node_) {
                    failed_heartbeats[node] = 0;
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lock(slot_map_mutex_);
                        changed = slot_map_->GetNodeState(node) != ClusterNodeState::Healthy;
                        slot_map_->MarkNodeHealthy(node);
                    }
                    if (changed) saveClusterConfigIfNeeded();
                    continue;
                }

                bool ok = pingClusterNode(node, config_.requirepass);
                bool changed = false;
                if (ok) {
                    failed_heartbeats[node] = 0;
                    {
                        std::lock_guard<std::mutex> lock(slot_map_mutex_);
                        changed = slot_map_->GetNodeState(node) != ClusterNodeState::Healthy;
                        slot_map_->MarkNodeHealthy(node);
                    }
                    ClusterSlotMapSnapshot remote_snapshot;
                    if (fetchClusterSlotMap(node, config_.requirepass, remote_snapshot)) {
                        bool loaded = false;
                        uint64_t previous_epoch = 0;
                        {
                            std::lock_guard<std::mutex> lock(slot_map_mutex_);
                            previous_epoch = slot_map_->GetEpoch();
                            loaded = slot_map_->LoadSnapshotIfNewer(remote_snapshot);
                        }
                        if (loaded) {
                            MINIREDIS_LOG_INFO("cluster")
                                << "loaded slot map from " << node
                                << " epoch " << previous_epoch
                                << " -> " << remote_snapshot.epoch;
                            saveClusterConfigIfNeeded();
                        }
                    }
                } else {
                    int failures = ++failed_heartbeats[node];
                    ClusterNodeState next_state = failures >= config_.cluster_fail_threshold
                                                      ? ClusterNodeState::Fail
                                                      : ClusterNodeState::Suspect;
                    std::lock_guard<std::mutex> lock(slot_map_mutex_);
                    changed = slot_map_->GetNodeState(node) != next_state;
                    if (next_state == ClusterNodeState::Fail) {
                        slot_map_->MarkNodeFailed(node);
                    } else {
                        slot_map_->MarkNodeSuspect(node);
                    }
                }
                if (changed) saveClusterConfigIfNeeded();
            }
        }
    });
}

bool MiniRedisServer::syncFromMaster() {
    if (config_.replicaof.empty()) return true;

    std::string host;
    int port = 0;
    if (!splitNodeAddr(config_.replicaof, host, port)) {
        MINIREDIS_LOG_ERROR("replication") << "invalid replicaof address: " << config_.replicaof;
        return false;
    }

    uint64_t last_offset = loadReplicationOffset();
    replication_offset_.store(last_offset, std::memory_order_relaxed);
    if (last_offset > 0 && tryPartialSyncFromMaster(host, port, last_offset)) {
        return true;
    }

    return fullSyncFromMaster(host, port);
}

bool MiniRedisServer::tryPartialSyncFromMaster(const std::string& host, int port,
                                               uint64_t last_offset) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to create replication socket: "
                                          << std::strerror(errno);
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to connect master "
                                          << config_.replicaof << ": " << std::strerror(errno);
        close(fd);
        return false;
    }

    std::string line;
    if (!config_.requirepass.empty()) {
        if (!writeAllBlocking(fd, encodeRespCommand({"AUTH", config_.requirepass})) ||
            !readRespLine(fd, line) || line.rfind("+OK", 0) != 0) {
            MINIREDIS_LOG_WARN("replication") << "master auth failed during partial sync";
            close(fd);
            return false;
        }
    }

    RespValue response;
    if (!writeAllBlocking(fd, encodeRespCommand({"REPLPSYNC", std::to_string(last_offset)})) ||
        !readRespValueBlocking(fd, response)) {
        MINIREDIS_LOG_WARN("replication") << "failed to read partial sync response";
        close(fd);
        return false;
    }
    close(fd);

    if (response.type != RespType::ARRAY || response.array.size() < 2 ||
        response.array[0].type != RespType::BULK_STRING ||
        response.array[1].type != RespType::INTEGER ||
        response.array[1].integer < 0) {
        MINIREDIS_LOG_WARN("replication") << "invalid partial sync response";
        return false;
    }

    std::string status = response.array[0].str;
    std::transform(status.begin(), status.end(), status.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (status == "FULLRESYNC") {
        MINIREDIS_LOG_INFO("replication") << "master requested full resync";
        return false;
    }
    if (status != "CONTINUE" || response.array.size() != 3 ||
        response.array[2].type != RespType::ARRAY) {
        MINIREDIS_LOG_WARN("replication") << "invalid partial sync status";
        return false;
    }

    uint64_t applied_offset = last_offset;
    for (const auto& item : response.array[2].array) {
        if (item.type != RespType::ARRAY || item.array.size() != 2 ||
            item.array[0].type != RespType::INTEGER ||
            item.array[0].integer < 0 ||
            item.array[1].type != RespType::ARRAY) {
            MINIREDIS_LOG_WARN("replication") << "invalid backlog entry";
            return false;
        }
        uint64_t offset = static_cast<uint64_t>(item.array[0].integer);
        std::vector<std::string> command;
        command.reserve(item.array[1].array.size());
        for (const auto& arg : item.array[1].array) {
            if (arg.type != RespType::BULK_STRING && arg.type != RespType::SIMPLE_STRING) {
                MINIREDIS_LOG_WARN("replication") << "invalid backlog command argument";
                return false;
            }
            command.push_back(arg.str);
        }
        if (!applyReplicationCommand(command, offset)) return false;
        applied_offset = offset;
    }

    uint64_t master_offset = static_cast<uint64_t>(response.array[1].integer);
    replication_offset_.store(master_offset, std::memory_order_relaxed);
    saveReplicationOffset(master_offset);
    MINIREDIS_LOG_INFO("replication") << "partial sync applied offset "
                                      << applied_offset << " -> " << master_offset;
    return true;
}

bool MiniRedisServer::fullSyncFromMaster(const std::string& host, int port) {

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to create replication socket: "
                                          << std::strerror(errno);
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to connect master "
                                          << config_.replicaof << ": " << std::strerror(errno);
        close(fd);
        return false;
    }

    std::string line;
    if (!config_.requirepass.empty()) {
        if (!writeAllBlocking(fd, encodeRespCommand({"AUTH", config_.requirepass})) ||
            !readRespLine(fd, line) || line.rfind("+OK", 0) != 0) {
            MINIREDIS_LOG_WARN("replication") << "master auth failed during full sync";
            close(fd);
            return false;
        }
    }

    RespValue response;
    if (!writeAllBlocking(fd, encodeRespCommand({"REPLFULLSYNC"})) ||
        !readRespValueBlocking(fd, response)) {
        MINIREDIS_LOG_WARN("replication") << "failed to read full snapshot from master";
        close(fd);
        return false;
    }
    close(fd);

    if (response.type == RespType::ERROR) {
        MINIREDIS_LOG_WARN("replication") << "master rejected full sync: " << response.str;
        return false;
    }
    uint64_t master_offset = 0;
    RespValue snapshot_response;
    if (response.type == RespType::ARRAY && response.array.size() == 2 &&
        response.array[0].type == RespType::INTEGER && response.array[0].integer >= 0 &&
        response.array[1].type == RespType::ARRAY) {
        master_offset = static_cast<uint64_t>(response.array[0].integer);
        snapshot_response = response.array[1];
    } else if (response.type == RespType::ARRAY) {
        snapshot_response = response;
    } else {
        MINIREDIS_LOG_WARN("replication") << "invalid full sync response type";
        return false;
    }
    constexpr size_t kMaxReplicationSnapshotEntries = 1'000'000;
    if (snapshot_response.array.size() > kMaxReplicationSnapshotEntries) {
        MINIREDIS_LOG_WARN("replication") << "full sync snapshot too large: "
                                          << snapshot_response.array.size();
        return false;
    }

    SnapshotData data;
    data.reserve(snapshot_response.array.size());
    for (const auto& item : snapshot_response.array) {
        if (item.type != RespType::ARRAY || item.array.size() != 3 ||
            item.array[0].type != RespType::BULK_STRING ||
            item.array[1].type != RespType::BULK_STRING ||
            item.array[2].type != RespType::INTEGER ||
            item.array[2].integer < 0) {
            MINIREDIS_LOG_WARN("replication") << "invalid full sync snapshot entry";
            return false;
        }
        data[item.array[0].str] = SnapshotEntry{
            item.array[1].str,
            static_cast<uint64_t>(item.array[2].integer),
        };
    }

    cache_.load_snapshot(data);
    replication_offset_.store(master_offset, std::memory_order_relaxed);
    saveReplicationOffset(master_offset);
    Stats::instance().setKeyCount(cache_.key_count());
    MINIREDIS_LOG_INFO("replication") << "full sync loaded " << data.size()
                                      << " keys from master " << config_.replicaof
                                      << ", offset=" << master_offset;
    if (aof_ && !aof_->rewrite(cache_.snapshot())) {
        MINIREDIS_LOG_WARN("replication") << "failed to compact local AOF after full sync";
    }
    return true;
}

bool MiniRedisServer::applyReplicationCommand(const std::vector<std::string>& command,
                                              uint64_t offset) {
    if (command.empty()) return false;
    std::string cmd = command[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (cmd == "REPLSET") {
        if (command.size() != 4) return false;
        size_t ttl_seconds_size = 0;
        auto [ptr, ec] = std::from_chars(command[3].data(), command[3].data() + command[3].size(),
                                         ttl_seconds_size);
        if (ec != std::errc() || ptr != command[3].data() + command[3].size() ||
            ttl_seconds_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        SetResult result = cache_.set(command[1], command[2], static_cast<int>(ttl_seconds_size));
        if (result != SetResult::Ok) return false;
        if (aof_ && !aof_->appendSet(command[1], command[2], static_cast<int>(ttl_seconds_size))) {
            return false;
        }
    } else if (cmd == "REPLDEL") {
        if (command.size() < 2) return false;
        std::vector<std::string> deleted;
        for (size_t i = 1; i < command.size(); ++i) {
            if (cache_.del(command[i])) deleted.push_back(command[i]);
        }
        if (aof_ && !deleted.empty() && !aof_->appendDel(deleted)) return false;
    } else if (cmd == "REPLEXPIRE") {
        if (command.size() != 3) return false;
        int ttl_seconds = 0;
        auto [ptr, ec] = std::from_chars(command[2].data(), command[2].data() + command[2].size(),
                                         ttl_seconds);
        if (ec != std::errc() || ptr != command[2].data() + command[2].size() ||
            ttl_seconds <= 0) {
            return false;
        }
        bool updated = cache_.expire(command[1], ttl_seconds);
        if (aof_ && updated && !aof_->appendExpire(command[1], ttl_seconds)) return false;
    } else {
        return false;
    }

    replication_offset_.store(offset, std::memory_order_relaxed);
    saveReplicationOffset(offset);
    Stats::instance().setKeyCount(cache_.key_count());
    return true;
}

std::string MiniRedisServer::replicationStateFile() const {
    if (!config_.snapshot_file.empty()) return config_.snapshot_file + ".repl.offset";
    return "snapshot_" + std::to_string(config_.port) + ".dat.repl.offset";
}

uint64_t MiniRedisServer::loadReplicationOffset() const {
    std::ifstream in(replicationStateFile());
    uint64_t offset = 0;
    if (in >> offset) return offset;
    return 0;
}

void MiniRedisServer::saveReplicationOffset(uint64_t offset) const {
    if (config_.replicaof.empty()) return;
    std::filesystem::path path(replicationStateFile());
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::trunc);
    if (out) out << offset << "\n";
}

int MiniRedisServer::run() {
    running_ = true;
    Stats::instance().setReady(false);
    Stats::instance().setIoThreads(config_.io_threads);
    g_running_signal_target = &running_;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (!configureCluster()) return 1;

    listen_fd_ = bindListen();
    if (listen_fd_ < 0) return 1;

    MINIREDIS_LOG_INFO("server") << "MiniRedis server started (RESP ready)";
    if (!config_.requirepass.empty() || !config_.acl_users.empty()) {
        MINIREDIS_LOG_INFO("server") << "AUTH/ACL enabled";
    }
    cache_.configure_memory_limit(config_.maxmemory_bytes, parseEvictionPolicy(config_.eviction_policy));
    Stats::instance().configureSlowLog(config_.slowlog_log_slower_than_us, config_.slowlog_max_len);
    if (config_.maxmemory_bytes > 0) {
        MINIREDIS_LOG_INFO("server") << "maxmemory enabled: " << config_.maxmemory_bytes
                                     << " bytes, policy=" << config_.eviction_policy;
    }
    MINIREDIS_LOG_INFO("server") << "io threads=" << config_.io_threads
                                 << ", cache shards=" << config_.cache_shards;

    startStatsServer();
    startClusterDiscovery();

    PersistenceManager persistence(cache_, config_.snapshot_file, thread_pool_,
                                   config_.snapshot_interval_sec);
    persistence.start();

    if (!config_.appendonly_file.empty()) {
        AppendFsyncPolicy appendfsync = AppendFsyncPolicy::EverySec;
        if (!parseAppendFsyncPolicy(config_.appendfsync, appendfsync)) {
            MINIREDIS_LOG_ERROR("aof") << "invalid appendfsync policy: " << config_.appendfsync;
            return 1;
        }
        aof_ = std::make_unique<AppendOnlyFile>(config_.appendonly_file, appendfsync);
        SnapshotData replay_data = cache_.snapshot();
        if (!aof_->replay(replay_data)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to replay AOF, startup aborted";
            return 1;
        }
        cache_.load_snapshot(replay_data);
        if (!aof_->open()) {
            MINIREDIS_LOG_ERROR("aof") << "failed to open AOF, startup aborted";
            return 1;
        }
    }

    if (!config_.replicaof.empty() && !syncFromMaster()) {
        MINIREDIS_LOG_WARN("replication")
            << "full sync from master failed; replica will continue with local state";
    }

    ReplicationBacklog* active_backlog = config_.replicaof.empty() ? &replication_backlog_ : nullptr;
    CommandHandler command_handler(cache_, memory_pool_, config_, config_.cluster_mode,
                                   current_node_, slot_map_.get(), &slot_map_mutex_,
                                   aof_.get(),
                                   [this]() { saveClusterConfigIfNeeded(); },
                                   active_backlog,
                                   &replication_offset_,
                                   [this](uint64_t offset) { saveReplicationOffset(offset); });
    command_handler.refreshRuntimeStats();

    io_schedulers_.reserve(config_.io_threads);
    io_threads_.reserve(config_.io_threads);
    for (size_t i = 0; i < config_.io_threads; ++i) {
        io_schedulers_.push_back(std::make_unique<Scheduler>());
    }
    for (auto& scheduler : io_schedulers_) {
        io_threads_.emplace_back([scheduler = scheduler.get()]() { scheduler->start(); });
    }

    accept_scheduler_.schedule_task(acceptLoop(accept_scheduler_, io_schedulers_,
                                               next_io_scheduler_, command_handler,
                                               config_, running_, listen_fd_));
    accept_thread_ = std::thread([this]() { accept_scheduler_.start(); });
    Stats::instance().setReady(true);

    while (running_.load()) {
        command_handler.refreshRuntimeStats();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    persistence.stop();
    stop();
    return 0;
}

void MiniRedisServer::stop() {
    running_ = false;
    stats_running_ = false;
    Stats::instance().setReady(false);

#ifdef HAVE_MYSQL
    if (config_.cluster_mode && mysql_) {
        mysql_->unregisterNode(current_node_);
    }
#endif
    if (cluster_refresh_thread_.joinable()) cluster_refresh_thread_.join();

    accept_scheduler_.stop();
    if (accept_thread_.joinable()) accept_thread_.join();

    for (auto& scheduler : io_schedulers_) {
        scheduler->stop();
    }
    for (auto& thread : io_threads_) {
        if (thread.joinable()) thread.join();
    }
    io_threads_.clear();
    io_schedulers_.clear();

    if (stats_thread_.joinable()) stats_thread_.join();

    thread_pool_.stop();
    if (aof_) {
        aof_->close();
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    //防止 MiniRedisServer 对象销毁后，信号处理函数还持有一个已经失效的指针。
    if (g_running_signal_target == &running_) {
        g_running_signal_target = nullptr;
    }
}

} // namespace miniredis
