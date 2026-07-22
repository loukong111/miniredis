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
    const std::string_view port_text(node.data() + pos + 1, node.size() - pos - 1);
    auto [ptr, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    return ec == std::errc() && ptr == port_text.data() + port_text.size() &&
           port > 0 && port <= 65535;
}

std::vector<std::string> splitReplicaNodes(const std::string& nodes) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    size_t start = 0;
    while (start <= nodes.size()) {
        size_t end = nodes.find(',', start);
        std::string node = nodes.substr(start, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - start);
        size_t first = node.find_first_not_of(" \t\r\n");
        size_t last = node.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            node = node.substr(first, last - first + 1);
            if (seen.insert(node).second) result.push_back(std::move(node));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
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
            if (decoder.hasProtocolError()) return false;
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
    addr.sin_port = htons(static_cast<uint16_t>(port));
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
    addr.sin_port = htons(static_cast<uint16_t>(port));
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

struct FailoverPeerStatus {
    std::string node;
    bool primary = false;
    std::string master;
    std::string replid;
    uint64_t offset = 0;
    uint64_t current_epoch = 0;
    uint64_t leader_epoch = 0;
    std::string leader;
    bool master_failed = false;
    bool writes_allowed = false;
};

bool sendCommandToNode(const std::string& node, const std::string& password,
                       const std::vector<std::string>& command, RespValue& response) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(node, host, port)) return false;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    timeval timeout{};
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    bool ok = inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1 &&
              connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    std::string line;
    if (ok && !password.empty()) {
        ok = writeAllBlocking(fd, encodeRespCommand({"AUTH", password})) &&
             readRespLine(fd, line) && line.rfind("+OK", 0) == 0;
    }
    if (ok) {
        ok = writeAllBlocking(fd, encodeRespCommand(command)) &&
             readRespValueBlocking(fd, response);
    }
    close(fd);
    return ok;
}

bool parseFailoverStatus(const RespValue& response, FailoverPeerStatus& status) {
    if (response.type != RespType::ARRAY || response.array.size() != 10) return false;
    const auto& item = response.array;
    if (item[0].type != RespType::BULK_STRING ||
        item[1].type != RespType::BULK_STRING ||
        item[2].type != RespType::BULK_STRING ||
        item[3].type != RespType::BULK_STRING ||
        item[4].type != RespType::INTEGER || item[4].integer < 0 ||
        item[5].type != RespType::INTEGER || item[5].integer < 0 ||
        item[6].type != RespType::INTEGER || item[6].integer < 0 ||
        item[7].type != RespType::BULK_STRING ||
        item[8].type != RespType::INTEGER ||
        item[9].type != RespType::INTEGER) {
        return false;
    }
    status.node = item[0].str;
    if (item[1].str == "master") {
        status.primary = true;
    } else if (item[1].str == "replica") {
        status.primary = false;
    } else {
        return false;
    }
    status.master = item[2].str;
    status.replid = item[3].str;
    status.offset = static_cast<uint64_t>(item[4].integer);
    status.current_epoch = static_cast<uint64_t>(item[5].integer);
    status.leader_epoch = static_cast<uint64_t>(item[6].integer);
    status.leader = item[7].str;
    status.master_failed = item[8].integer == 1;
    status.writes_allowed = item[9].integer == 1;
    return (item[8].integer == 0 || item[8].integer == 1) &&
           (item[9].integer == 0 || item[9].integer == 1);
}

bool queryFailoverStatus(const std::string& node, const std::string& password,
                         FailoverPeerStatus& status) {
    RespValue response;
    return sendCommandToNode(node, password, {"REPLFAILOVER", "STATUS"}, response) &&
           parseFailoverStatus(response, status) && status.node == node;
}

bool isSimpleOk(const RespValue& response) {
    return response.type == RespType::SIMPLE_STRING && response.str == "OK";
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
                if (!opt_cmd) {
                    if (decoder.hasProtocolError()) {
                        outbuf = RespWriter::error("Protocol error: " + decoder.protocolError());
                        close_after_write = true;
                        writing = true;
                    }
                    break;
                }
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
      next_io_scheduler_(0),
      running_(true),
      stats_running_(false),
      listen_fd_(-1),
      local_replid_(generateReplicationId()),
      replication_backlog_(config_.replication_backlog_size),
      replication_offset_(0),
      replica_following_(!config_.replicaof.empty()),
      primary_state_(config_.replicaof.empty()),
      writes_allowed_(!config_.automatic_failover),
      active_master_(config_.replicaof) {}

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

bool MiniRedisServer::configureFailover() {
    if (!config_.automatic_failover) return true;

    if (config_.requirepass.empty() && !config_.acl_users.empty()) {
        MINIREDIS_LOG_ERROR("failover")
            << "automatic failover with ACL requires a shared --requirepass for node authentication";
        return false;
    }
    if (config_.node_addr.empty()) {
        MINIREDIS_LOG_ERROR("failover")
            << "automatic failover requires --node-addr so peers have a stable identity";
        return false;
    }
    if (!config_.cluster_mode) {
        int advertised_port = 0;
        if (!parseNodePort(config_.node_addr, advertised_port) || advertised_port != config_.port) {
            MINIREDIS_LOG_ERROR("failover")
                << "--node-addr must be a valid ip:port matching --port";
            return false;
        }
        current_node_ = config_.node_addr;
        Stats::instance().setNodeAddr(current_node_);
    }

    failover_nodes_ = splitReplicaNodes(config_.failover_nodes_str);
    if (failover_nodes_.size() < 3 ||
        std::find(failover_nodes_.begin(), failover_nodes_.end(), current_node_) ==
            failover_nodes_.end()) {
        MINIREDIS_LOG_ERROR("failover")
            << "failover group must contain this node and at least three unique members";
        return false;
    }
    for (const auto& node : failover_nodes_) {
        std::string host;
        int port = 0;
        if (!splitNodeAddr(node, host, port)) {
            MINIREDIS_LOG_ERROR("failover") << "invalid failover node: " << node;
            return false;
        }
    }
    if (!config_.replicaof.empty() &&
        std::find(failover_nodes_.begin(), failover_nodes_.end(), config_.replicaof) ==
            failover_nodes_.end()) {
        MINIREDIS_LOG_ERROR("failover") << "replicaof master is missing from failover group";
        return false;
    }

    if (config_.failover_state_file.empty()) {
        config_.failover_state_file = config_.snapshot_file + ".failover.state";
    }
    failover_coordinator_ = std::make_unique<FailoverCoordinator>(
        current_node_, config_.failover_state_file);
    if (!failover_coordinator_->restore()) {
        MINIREDIS_LOG_ERROR("failover") << "failed to restore failover state: "
                                        << config_.failover_state_file;
        return false;
    }

    const FailoverState restored = failover_coordinator_->snapshot();
    if (!restored.leader.empty()) {
        if (std::find(failover_nodes_.begin(), failover_nodes_.end(), restored.leader) ==
            failover_nodes_.end()) {
            MINIREDIS_LOG_ERROR("failover") << "persisted leader is outside failover group";
            return false;
        }
        active_master_ = restored.leader;
        const bool primary = restored.leader == current_node_;
        primary_state_.store(primary, std::memory_order_relaxed);
        replica_following_.store(!primary, std::memory_order_relaxed);
    } else {
        active_master_ = config_.replicaof.empty() ? current_node_ : config_.replicaof;
        primary_state_.store(config_.replicaof.empty(), std::memory_order_relaxed);
        replica_following_.store(!config_.replicaof.empty(), std::memory_order_relaxed);
    }
    writes_allowed_.store(false, std::memory_order_relaxed);
    updateFailoverStats(1);
    MINIREDIS_LOG_INFO("failover") << "automatic failover enabled with "
                                   << failover_nodes_.size() << " voters";
    return true;
}

std::string MiniRedisServer::activeMaster() const {
    std::lock_guard<std::mutex> lock(failover_runtime_mutex_);
    return active_master_;
}

size_t MiniRedisServer::transferClusterSlots(const std::string& old_master,
                                             const std::string& new_master,
                                             uint64_t epoch) {
    if (!config_.cluster_mode || !slot_map_) return 0;
    size_t moved = 0;
    {
        std::lock_guard<std::mutex> lock(slot_map_mutex_);
        moved = slot_map_->TakeoverNodeSlots(old_master, new_master, epoch);
    }
    if (moved > 0) saveClusterConfigIfNeeded();
    return moved;
}

std::string MiniRedisServer::failoverInfo() const {
    if (!config_.automatic_failover || !failover_coordinator_) {
        return "automatic_failover_enabled:0\r\n";
    }
    const FailoverState state = failover_coordinator_->snapshot();
    std::ostringstream output;
    output << "automatic_failover_enabled:1\r\n"
           << "failover_group_size:" << failover_nodes_.size() << "\r\n"
           << "failover_quorum:" << (failover_nodes_.size() / 2 + 1) << "\r\n"
           << "failover_current_epoch:" << state.current_epoch << "\r\n"
           << "failover_last_voted_epoch:" << state.last_voted_epoch << "\r\n"
           << "failover_voted_for:" << state.voted_for << "\r\n"
           << "failover_leader_epoch:" << state.leader_epoch << "\r\n"
           << "failover_leader:" << state.leader << "\r\n"
           << "failover_master_failed:"
           << (master_failed_.load(std::memory_order_acquire) ? "1" : "0") << "\r\n"
           << "failover_writes_allowed:"
           << (writes_allowed_.load(std::memory_order_acquire) ? "1" : "0") << "\r\n";
    return output.str();
}

void MiniRedisServer::updateFailoverStats(size_t reachable_nodes) const {
    if (!config_.automatic_failover || !failover_coordinator_) {
        Stats::instance().setFailoverState(false, 0, 0, 0, 0, 0, false, false);
        return;
    }
    const FailoverState state = failover_coordinator_->snapshot();
    Stats::instance().setFailoverState(
        true, failover_nodes_.size(), failover_nodes_.size() / 2 + 1,
        reachable_nodes, state.current_epoch, state.leader_epoch,
        master_failed_.load(std::memory_order_acquire),
        writes_allowed_.load(std::memory_order_acquire));
}

std::string MiniRedisServer::handleFailoverCommand(const RespValue& command) {
    if (!config_.automatic_failover || !failover_coordinator_) {
        return RespWriter::error("automatic failover is not enabled");
    }
    if (command.type != RespType::ARRAY || command.array.size() < 2 ||
        command.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("invalid REPLFAILOVER command");
    }

    std::string subcommand = command.array[1].str;
    std::transform(subcommand.begin(), subcommand.end(), subcommand.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    auto is_member = [this](const std::string& node) {
        return std::find(failover_nodes_.begin(), failover_nodes_.end(), node) !=
               failover_nodes_.end();
    };
    auto parse_uint64 = [](const RespValue& value, uint64_t& parsed) {
        if (value.type != RespType::BULK_STRING || value.str.empty()) return false;
        const char* end = value.str.data() + value.str.size();
        auto [ptr, ec] = std::from_chars(value.str.data(), end, parsed);
        return ec == std::errc() && ptr == end;
    };

    if (subcommand == "STATUS") {
        if (command.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'replfailover status'");
        }
        const bool primary = primary_state_.load(std::memory_order_acquire);
        std::string replid;
        uint64_t offset = 0;
        if (primary) {
            replid = local_replid_;
            offset = replication_backlog_.currentOffset();
        } else {
            std::lock_guard<std::mutex> lock(replication_apply_mutex_);
            replid = upstream_replid_;
            offset = replication_offset_.load(std::memory_order_relaxed);
        }
        const FailoverState state = failover_coordinator_->snapshot();
        std::string response = "*10\r\n";
        response += RespWriter::bulkString(current_node_);
        response += RespWriter::bulkString(primary ? "master" : "replica");
        response += RespWriter::bulkString(primary ? "" : activeMaster());
        response += RespWriter::bulkString(replid);
        response += RespWriter::integer(static_cast<long long>(offset));
        response += RespWriter::integer(static_cast<long long>(state.current_epoch));
        response += RespWriter::integer(static_cast<long long>(state.leader_epoch));
        response += RespWriter::bulkString(state.leader);
        response += RespWriter::integer(master_failed_.load(std::memory_order_acquire) ? 1 : 0);
        response += RespWriter::integer(writes_allowed_.load(std::memory_order_acquire) ? 1 : 0);
        return response;
    }

    if (subcommand == "VOTE") {
        if (command.array.size() != 7) {
            return RespWriter::error("wrong number of arguments for 'replfailover vote'");
        }
        uint64_t epoch = 0;
        uint64_t candidate_offset = 0;
        if (!parse_uint64(command.array[2], epoch) ||
            command.array[3].type != RespType::BULK_STRING ||
            command.array[4].type != RespType::BULK_STRING ||
            command.array[5].type != RespType::BULK_STRING ||
            !parse_uint64(command.array[6], candidate_offset)) {
            return RespWriter::error("invalid failover vote arguments");
        }
        const std::string& candidate = command.array[3].str;
        const std::string& failed_master = command.array[4].str;
        const std::string& replid = command.array[5].str;
        if (!is_member(candidate) || !is_member(failed_master) ||
            activeMaster() != failed_master ||
            primary_state_.load(std::memory_order_acquire)) {
            return RespWriter::error("failover vote topology mismatch");
        }

        uint64_t local_offset = 0;
        {
            std::lock_guard<std::mutex> lock(replication_apply_mutex_);
            if (upstream_replid_ != replid || !isValidReplicationId(replid)) {
                return RespWriter::error("failover vote replication ID mismatch");
            }
            local_offset = replication_offset_.load(std::memory_order_relaxed);
        }
        if (!failover_coordinator_->grantVote(
                epoch, candidate, candidate_offset, local_offset,
                master_failed_.load(std::memory_order_acquire))) {
            return RespWriter::error("failover vote rejected");
        }
        return RespWriter::simpleString("OK");
    }

    if (subcommand == "ANNOUNCE") {
        if (command.array.size() != 6) {
            return RespWriter::error("wrong number of arguments for 'replfailover announce'");
        }
        uint64_t epoch = 0;
        if (!parse_uint64(command.array[2], epoch) ||
            command.array[3].type != RespType::BULK_STRING ||
            command.array[4].type != RespType::BULK_STRING ||
            command.array[5].type != RespType::BULK_STRING) {
            return RespWriter::error("invalid failover announcement");
        }
        const std::string& leader = command.array[3].str;
        const std::string& old_master = command.array[4].str;
        const FailoverState previous = failover_coordinator_->snapshot();
        if (!is_member(leader) || !is_member(old_master) ||
            !isValidReplicationId(command.array[5].str) ||
            !failover_coordinator_->recordLeader(epoch, leader)) {
            return RespWriter::error("failover announcement rejected");
        }
        const bool already_following =
            previous.leader_epoch == epoch && previous.leader == leader &&
            ((leader == current_node_ && primary_state_.load(std::memory_order_acquire)) ||
             (leader != current_node_ && !primary_state_.load(std::memory_order_acquire) &&
              activeMaster() == leader));
        if (!already_following) adoptFailoverLeader(epoch, leader, old_master);
        return RespWriter::simpleString("OK");
    }

    return RespWriter::error("unsupported REPLFAILOVER subcommand");
}

void MiniRedisServer::promoteAfterElection(uint64_t epoch, const std::string& old_master) {
    std::lock_guard<std::mutex> transition_lock(failover_transition_mutex_);
    if (primary_state_.exchange(true, std::memory_order_acq_rel)) return;
    replica_following_.store(false, std::memory_order_release);
    master_failed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(failover_runtime_mutex_);
        active_master_ = current_node_;
    }
    replication_backlog_.reset();
    if (replication_dispatcher_) {
        replication_dispatcher_->stop();
        replication_dispatcher_->setMasterReplicationId(local_replid_);
        replication_dispatcher_->start();
    }
    const size_t moved_slots = transferClusterSlots(old_master, current_node_, epoch);
    writes_allowed_.store(true, std::memory_order_release);
    Stats::instance().recordFailoverElection();
    updateFailoverStats(failover_nodes_.size() / 2 + 1);
    MINIREDIS_LOG_WARN("failover") << "elected " << current_node_ << " as master at epoch "
                                   << epoch << ", took over " << moved_slots << " slots";
}

void MiniRedisServer::adoptFailoverLeader(uint64_t epoch, const std::string& leader,
                                          const std::string& old_master) {
    if (leader == current_node_) {
        promoteAfterElection(epoch, old_master);
        return;
    }
    std::lock_guard<std::mutex> transition_lock(failover_transition_mutex_);
    if (replication_dispatcher_) replication_dispatcher_->stop();
    writes_allowed_.store(false, std::memory_order_release);
    primary_state_.store(false, std::memory_order_release);
    replica_following_.store(true, std::memory_order_release);
    master_failed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(failover_runtime_mutex_);
        active_master_ = leader;
    }
    {
        std::lock_guard<std::mutex> lock(replication_apply_mutex_);
        upstream_replid_.clear();
        replication_offset_.store(0, std::memory_order_relaxed);
        replication_state_loaded_ = true;
    }
    const size_t moved_slots = transferClusterSlots(old_master, leader, epoch);
    updateFailoverStats(1);
    MINIREDIS_LOG_WARN("failover") << current_node_ << " follows elected master " << leader
                                   << " at epoch " << epoch << ", updated "
                                   << moved_slots << " slots";
}

void MiniRedisServer::saveClusterConfigIfNeeded() const {
    if (!config_.cluster_mode || !slot_map_ || config_.cluster_config_file.empty()) return;
    std::lock_guard<std::mutex> lock(cluster_config_save_mutex_);
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
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
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

        if (mysql_ && !mysql_->registerNode(current_node_)) {
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
                if (!mysql_->registerNode(current_node_)) {
                    MINIREDIS_LOG_WARN("cluster") << "failed to refresh cluster node heartbeat";
                }
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

void MiniRedisServer::startFailoverMonitor() {
    if (!config_.automatic_failover || !failover_coordinator_) return;

    failover_thread_ = std::thread([this]() {
        int failed_master_heartbeats = 0;
        const size_t quorum = failover_nodes_.size() / 2 + 1;
        const auto interval = std::chrono::milliseconds(config_.failover_heartbeat_interval_ms);

        while (running_.load(std::memory_order_acquire)) {
            const auto deadline = std::chrono::steady_clock::now() + interval;
            while (running_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!running_.load(std::memory_order_acquire)) break;

            std::vector<FailoverPeerStatus> peers;
            peers.reserve(failover_nodes_.size() - 1);
            for (const auto& node : failover_nodes_) {
                if (node == current_node_) continue;
                FailoverPeerStatus status;
                if (queryFailoverStatus(node, config_.requirepass, status)) {
                    peers.push_back(std::move(status));
                }
            }

            FailoverState local_state = failover_coordinator_->snapshot();
            const FailoverPeerStatus* newest_leader = nullptr;
            for (const auto& peer : peers) {
                if (peer.leader.empty() ||
                    std::find(failover_nodes_.begin(), failover_nodes_.end(), peer.leader) ==
                        failover_nodes_.end()) {
                    continue;
                }
                if (!newest_leader || peer.leader_epoch > newest_leader->leader_epoch) {
                    newest_leader = &peer;
                }
            }
            if (newest_leader &&
                (newest_leader->leader_epoch > local_state.leader_epoch ||
                 (newest_leader->leader_epoch == local_state.leader_epoch &&
                  local_state.leader.empty()))) {
                const std::string old_master = activeMaster();
                if (failover_coordinator_->recordLeader(newest_leader->leader_epoch,
                                                        newest_leader->leader)) {
                    adoptFailoverLeader(newest_leader->leader_epoch,
                                        newest_leader->leader, old_master);
                    local_state = failover_coordinator_->snapshot();
                }
            }
            const size_t reachable_nodes = peers.size() + 1;
            updateFailoverStats(reachable_nodes);

            if (primary_state_.load(std::memory_order_acquire)) {
                const bool has_quorum = reachable_nodes >= quorum;
                const bool previous = writes_allowed_.exchange(has_quorum,
                                                               std::memory_order_acq_rel);
                if (previous != has_quorum) {
                    MINIREDIS_LOG_WARN("failover")
                        << (has_quorum ? "write quorum restored" : "write quorum lost; fencing writes")
                        << " (reachable=" << reachable_nodes << "/" << failover_nodes_.size()
                        << ", quorum=" << quorum << ")";
                }
                updateFailoverStats(reachable_nodes);

                local_state = failover_coordinator_->snapshot();
                if (local_state.leader == current_node_ && local_state.leader_epoch > 0 &&
                    !config_.replicaof.empty()) {
                    for (const auto& node : failover_nodes_) {
                        if (node == current_node_) continue;
                        RespValue ignored;
                        sendCommandToNode(node, config_.requirepass,
                                          {"REPLFAILOVER", "ANNOUNCE",
                                           std::to_string(local_state.leader_epoch),
                                           current_node_, config_.replicaof,
                                           local_replid_},
                                          ignored);
                    }
                }
                continue;
            }

            writes_allowed_.store(false, std::memory_order_release);
            const std::string failed_master = activeMaster();
            bool master_reachable = false;
            for (const auto& peer : peers) {
                if (peer.node == failed_master && peer.primary) {
                    master_reachable = true;
                    break;
                }
            }
            if (master_reachable) {
                failed_master_heartbeats = 0;
                master_failed_.store(false, std::memory_order_release);
                updateFailoverStats(reachable_nodes);
                continue;
            }

            ++failed_master_heartbeats;
            if (failed_master_heartbeats < config_.failover_fail_threshold) continue;
            master_failed_.store(true, std::memory_order_release);
            updateFailoverStats(reachable_nodes);

            std::string replid;
            uint64_t local_offset = 0;
            {
                std::lock_guard<std::mutex> lock(replication_apply_mutex_);
                replid = upstream_replid_;
                local_offset = replication_offset_.load(std::memory_order_relaxed);
            }
            if (!isValidReplicationId(replid)) continue;

            std::vector<const FailoverPeerStatus*> voters;
            uint64_t max_seen_epoch = failover_coordinator_->snapshot().current_epoch;
            std::string candidate = current_node_;
            uint64_t candidate_offset = local_offset;
            for (const auto& peer : peers) {
                max_seen_epoch = std::max(max_seen_epoch, peer.current_epoch);
                if (peer.primary || peer.node == failed_master || !peer.master_failed ||
                    peer.master != failed_master || peer.replid != replid) {
                    continue;
                }
                voters.push_back(&peer);
                if (peer.offset > candidate_offset ||
                    (peer.offset == candidate_offset && peer.node < candidate)) {
                    candidate = peer.node;
                    candidate_offset = peer.offset;
                }
            }
            if (voters.size() + 1 < quorum || candidate != current_node_) continue;

            const uint64_t epoch = failover_coordinator_->beginElection(max_seen_epoch);
            if (epoch == 0) {
                MINIREDIS_LOG_ERROR("failover") << "failed to persist new election epoch";
                continue;
            }
            size_t votes = 1;
            for (const auto* voter : voters) {
                RespValue response;
                if (sendCommandToNode(voter->node, config_.requirepass,
                                      {"REPLFAILOVER", "VOTE", std::to_string(epoch),
                                       current_node_, failed_master, replid,
                                       std::to_string(local_offset)},
                                      response) &&
                    isSimpleOk(response)) {
                    ++votes;
                }
            }
            if (votes < quorum) {
                MINIREDIS_LOG_WARN("failover") << "election epoch " << epoch
                                               << " received " << votes << "/" << quorum
                                               << " required votes";
                continue;
            }
            if (!failover_coordinator_->recordLeader(epoch, current_node_)) {
                MINIREDIS_LOG_ERROR("failover") << "failed to persist elected leader";
                continue;
            }

            promoteAfterElection(epoch, failed_master);
            for (const auto& node : failover_nodes_) {
                if (node == current_node_) continue;
                RespValue ignored;
                sendCommandToNode(node, config_.requirepass,
                                  {"REPLFAILOVER", "ANNOUNCE", std::to_string(epoch),
                                   current_node_, failed_master, local_replid_},
                                  ignored);
            }
        }
    });
}

bool MiniRedisServer::syncFromMaster() {
    if (!replica_following_.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> apply_lock(replication_apply_mutex_);

    std::string host;
    int port = 0;
    const std::string master = activeMaster();
    if (master.empty() || master == current_node_ || !splitNodeAddr(master, host, port)) {
        MINIREDIS_LOG_ERROR("replication") << "invalid active master address: " << master;
        return false;
    }

    if (!replication_state_loaded_) {
        ReplicationState state = loadReplicationState();
        upstream_replid_ = std::move(state.master_replid);
        replication_offset_.store(state.offset, std::memory_order_relaxed);
        replication_state_loaded_ = true;
    }

    const uint64_t last_offset = replication_offset_.load(std::memory_order_acquire);
    if (tryPartialSyncFromMaster(host, port, upstream_replid_, last_offset)) {
        return true;
    }

    return fullSyncFromMaster(host, port);
}

void MiniRedisServer::startReplicationSync() {
    if (config_.replicaof.empty() && !config_.automatic_failover) return;

    replication_sync_thread_ = std::thread([this]() {
        const auto sync_interval =
            std::chrono::milliseconds(config_.replication_sync_interval_ms);
        while (running_.load(std::memory_order_acquire)) {
            const auto deadline = std::chrono::steady_clock::now() + sync_interval;
            while (running_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_.load(std::memory_order_acquire)) break;
            if (!replica_following_.load(std::memory_order_acquire)) continue;
            if (!syncFromMaster()) {
                MINIREDIS_LOG_WARN("replication")
                    << "periodic replication catch-up from " << activeMaster()
                    << " failed; retrying";
            }
        }
    });
}

bool MiniRedisServer::tryPartialSyncFromMaster(const std::string& host, int port,
                                               const std::string& last_replid,
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
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to connect master "
                                          << activeMaster() << ": " << std::strerror(errno);
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
    const std::string requested_replid = isValidReplicationId(last_replid) ? last_replid : "?";
    if (!writeAllBlocking(fd, encodeRespCommand(
                                  {"REPLPSYNC", requested_replid,
                                   std::to_string(last_offset)})) ||
        !readRespValueBlocking(fd, response)) {
        MINIREDIS_LOG_WARN("replication") << "failed to read partial sync response";
        close(fd);
        return false;
    }
    close(fd);

    if (response.type != RespType::ARRAY || response.array.size() < 3 ||
        response.array[0].type != RespType::BULK_STRING ||
        response.array[1].type != RespType::BULK_STRING ||
        !isValidReplicationId(response.array[1].str) ||
        response.array[2].type != RespType::INTEGER ||
        response.array[2].integer < 0) {
        MINIREDIS_LOG_WARN("replication") << "invalid partial sync response";
        return false;
    }

    std::string status = response.array[0].str;
    std::transform(status.begin(), status.end(), status.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (status == "FULLRESYNC") {
        if (response.array.size() != 3) {
            MINIREDIS_LOG_WARN("replication") << "invalid FULLRESYNC response";
            return false;
        }
        MINIREDIS_LOG_INFO("replication")
            << "master requested full resync: requested_replid=" << requested_replid
            << ", current_replid=" << response.array[1].str;
        return false;
    }
    if (status != "CONTINUE" || response.array.size() != 4 ||
        response.array[1].str != requested_replid ||
        response.array[3].type != RespType::ARRAY) {
        MINIREDIS_LOG_WARN("replication") << "invalid partial sync status";
        return false;
    }

    const std::string& master_replid = response.array[1].str;
    upstream_replid_ = master_replid;
    uint64_t applied_offset = last_offset;
    for (const auto& item : response.array[3].array) {
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
        if (!applyReplicationCommand(command, master_replid, offset)) return false;
        applied_offset = offset;
    }

    uint64_t master_offset = static_cast<uint64_t>(response.array[2].integer);
    if (applied_offset != master_offset) {
        MINIREDIS_LOG_WARN("replication") << "partial sync ended at offset "
                                          << applied_offset << " but master reported "
                                          << master_offset;
        return false;
    }
    replication_offset_.store(master_offset, std::memory_order_relaxed);
    if (!saveReplicationState(master_offset)) {
        MINIREDIS_LOG_WARN("replication") << "failed to persist partial sync state";
    }
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
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        MINIREDIS_LOG_WARN("replication") << "failed to connect master "
                                          << activeMaster() << ": " << std::strerror(errno);
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
    if (response.type != RespType::ARRAY || response.array.size() != 4 ||
        response.array[0].type != RespType::BULK_STRING ||
        response.array[0].str != "FULLRESYNC" ||
        response.array[1].type != RespType::BULK_STRING ||
        !isValidReplicationId(response.array[1].str) ||
        response.array[2].type != RespType::INTEGER || response.array[2].integer < 0 ||
        response.array[3].type != RespType::ARRAY) {
        MINIREDIS_LOG_WARN("replication") << "invalid full sync response";
        return false;
    }
    const std::string master_replid = response.array[1].str;
    const uint64_t master_offset = static_cast<uint64_t>(response.array[2].integer);
    const RespValue& snapshot_response = response.array[3];
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
    upstream_replid_ = master_replid;
    replication_offset_.store(master_offset, std::memory_order_relaxed);
    if (!saveReplicationState(master_offset)) {
        MINIREDIS_LOG_WARN("replication") << "failed to persist full sync state";
    }
    Stats::instance().setKeyCount(cache_.key_count());
    MINIREDIS_LOG_INFO("replication") << "full sync loaded " << data.size()
                                      << " keys from master " << activeMaster()
                                      << ", replid=" << master_replid
                                      << ", offset=" << master_offset;
    if (aof_ && !aof_->rewrite(cache_.snapshot())) {
        MINIREDIS_LOG_WARN("replication") << "failed to compact local AOF after full sync";
    }
    return true;
}

bool MiniRedisServer::applyReplicationCommand(const std::vector<std::string>& command,
                                              const std::string& replid,
                                              uint64_t offset) {
    if (command.empty() || replid != upstream_replid_) return false;
    const uint64_t current_offset = replication_offset_.load(std::memory_order_acquire);
    if (offset <= current_offset) return true;
    if (offset != current_offset + 1) {
        MINIREDIS_LOG_WARN("replication") << "replication gap: expected offset "
                                          << current_offset + 1 << ", got " << offset;
        return false;
    }
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
    if (!saveReplicationState(offset)) {
        MINIREDIS_LOG_WARN("replication") << "failed to persist applied replication state";
    }
    Stats::instance().setKeyCount(cache_.key_count());
    return true;
}

std::string MiniRedisServer::replicationStateFile() const {
    if (!config_.snapshot_file.empty()) return config_.snapshot_file + ".repl.state";
    return "snapshot_" + std::to_string(config_.port) + ".dat.repl.state";
}

std::string MiniRedisServer::legacyReplicationOffsetFile() const {
    if (!config_.snapshot_file.empty()) return config_.snapshot_file + ".repl.offset";
    return "snapshot_" + std::to_string(config_.port) + ".dat.repl.offset";
}

ReplicationState MiniRedisServer::loadReplicationState() const {
    ReplicationState state;
    if (loadReplicationStateFile(replicationStateFile(), state)) {
        return state;
    }

    if (loadReplicationStateFile(legacyReplicationOffsetFile(), state)) {
        MINIREDIS_LOG_INFO("replication")
            << "loaded legacy offset-only replication state; full resync is required";
        return state;
    }
    return {};
}

bool MiniRedisServer::saveReplicationState(uint64_t offset) const {
    if (primary_state_.load(std::memory_order_acquire)) return true;
    return saveReplicationStateFile(replicationStateFile(),
                                    ReplicationState{upstream_replid_, offset});
}

int MiniRedisServer::run() {
    running_ = true;
    Stats::instance().setReady(false);
    Stats::instance().setIoThreads(config_.io_threads);
    g_running_signal_target = &running_;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (!configureCluster() || !configureFailover()) return 1;

    listen_fd_ = bindListen();
    if (listen_fd_ < 0) return 1;

    MINIREDIS_LOG_INFO("server") << "MiniRedis server started (RESP ready)";
    MINIREDIS_LOG_INFO("replication") << "local replication ID=" << local_replid_;
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

    if (replica_following_.load(std::memory_order_acquire) && !syncFromMaster()) {
        MINIREDIS_LOG_WARN("replication")
            << "full sync from master failed; replica will continue with local state";
    }

    std::vector<std::string> configured_replicas;
    if (config_.automatic_failover) {
        for (const auto& node : failover_nodes_) {
            if (node != current_node_) configured_replicas.push_back(node);
        }
    } else {
        configured_replicas = splitReplicaNodes(config_.replicas_str);
    }
    if (!configured_replicas.empty()) {
        replication_dispatcher_ = std::make_unique<ReplicationDispatcher>(
            replication_backlog_, local_replid_, configured_replicas, config_.requirepass,
            std::chrono::milliseconds(config_.replication_reconnect_delay_ms));
        if (primary_state_.load(std::memory_order_acquire)) replication_dispatcher_->start();
    }
    CommandHandler command_handler(cache_, memory_pool_, config_, config_.cluster_mode,
                                   current_node_, slot_map_.get(), &slot_map_mutex_,
                                   aof_.get(),
                                   [this]() { saveClusterConfigIfNeeded(); },
                                   &replication_backlog_,
                                   &replication_offset_,
                                   [this](uint64_t offset) {
                                       if (!saveReplicationState(offset)) {
                                           MINIREDIS_LOG_WARN("replication")
                                               << "failed to persist pushed replication state";
                                       }
                                   },
                                   replication_dispatcher_.get(),
                                   &replication_apply_mutex_,
                                   [this]() {
                                       std::lock_guard<std::mutex> transition_lock(
                                           failover_transition_mutex_);
                                       replica_following_.store(false, std::memory_order_release);
                                       replication_backlog_.reset();
                                       if (replication_dispatcher_) replication_dispatcher_->start();
                                   },
                                   &local_replid_,
                                   &upstream_replid_,
                                   &primary_state_,
                                   &writes_allowed_,
                                   [this]() { return activeMaster(); },
                                   [this](const RespValue& command) {
                                       return handleFailoverCommand(command);
                                   },
                                   [this]() { return failoverInfo(); });
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
    startReplicationSync();
    startFailoverMonitor();

    auto next_expiration_cleanup = std::chrono::steady_clock::now();
    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_expiration_cleanup) {
            cache_.cleanup();
            next_expiration_cleanup = now + std::chrono::seconds(1);
        }
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
    if (failover_thread_.joinable()) failover_thread_.join();

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

    if (replication_dispatcher_) replication_dispatcher_->stop();
    if (replication_sync_thread_.joinable()) replication_sync_thread_.join();

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
