#include "miniredis/server/server.hpp"
#include "miniredis/metrics/http_stats.hpp"
#include "miniredis/metrics/stats.hpp"
#include "miniredis/persistence/persistence_manager.hpp"
#include "miniredis/server/command_handler.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <unordered_map>
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

bool sendHeartbeatCommand(int fd, const std::vector<std::string>& command, const std::string& expected) {
    if (!writeAllBlocking(fd, encodeRespCommand(command))) return false;
    std::string line;
    if (!readRespLine(fd, line)) return false;
    return line.rfind(expected, 0) == 0;
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
    bool authenticated = config.requirepass.empty();

    while (true) {
        if (!writing) {
            co_await scheduler.await_io(client.fd(), EPOLLIN);
        }
        if (!writing) {
            ssize_t n = read(client.fd(), buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN) {
                    perror("read error");
                }
                client.closeNow();
                break;
            }

            decoder.feed(std::string_view(buffer, static_cast<size_t>(n)));
            if (decoder.bufferedSize() > config.max_request_bytes) {
                outbuf += RespWriter::error("request too large");
                client.closeNow();
                break;
            }

            while (true) {
                auto opt_cmd = decoder.parse();
                if (!opt_cmd) break;
                auto start = std::chrono::steady_clock::now();
                outbuf += command_handler.handle(*opt_cmd, authenticated);
                auto end = std::chrono::steady_clock::now();
                auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                Stats::instance().recordCommandLatency(static_cast<size_t>(latency_us));
            }
        }

        if (!outbuf.empty()) {
            ssize_t n = write(client.fd(), outbuf.data(), outbuf.size());
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    writing = true;
                    co_await scheduler.await_io(client.fd(), EPOLLOUT);
                    continue;
                }
                perror("write error");
                client.closeNow();
                break;
            }
            if (n == static_cast<ssize_t>(outbuf.size())) {
                outbuf.clear();
                writing = false;
            } else {
                outbuf.erase(0, static_cast<size_t>(n));
                writing = true;
                co_await scheduler.await_io(client_fd, EPOLLOUT);
            }
        } else {
            writing = false;
        }
    }
    co_return;
}

Task acceptLoop(Scheduler& scheduler, CommandHandler& command_handler,
                const AppConfig& config, std::atomic<bool>& running, int listen_fd) {
    while (running.load()) {
        co_await scheduler.await_io(listen_fd, EPOLLIN);
        while (true) {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    perror("accept");
                }
                break;
            }
            if (!setNonBlocking(client_fd)) {
                perror("client fcntl");
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
            scheduler.schedule_task(handleClient(scheduler, command_handler, config, client_fd));
        }
    }
}

} // namespace

MiniRedisServer::MiniRedisServer(AppConfig config)
    : config_(std::move(config)),
      memory_pool_(1024),
      thread_pool_(4, 16, 5),
      cache_(memory_pool_),
      running_(true),
      stats_running_(false),
      listen_fd_(-1) {}

MiniRedisServer::~MiniRedisServer() {
    stop();
}

bool MiniRedisServer::configureCluster() {
    if (config_.cluster_mode) {
        if (config_.node_addr.empty() || config_.nodes_str.empty()) {
            std::cerr << "Cluster mode requires --node-addr and --nodes" << std::endl;
            return false;
        }
        current_node_ = config_.node_addr;
        if (!parseNodePort(current_node_, config_.port)) {
            std::cerr << "Invalid --node-addr, expected ip:port" << std::endl;
            return false;
        }

        auto nodes = parseClusterNodes(config_.nodes_str);
        if (nodes.empty()) {
            std::cerr << "Cluster mode requires at least one node in --nodes" << std::endl;
            return false;
        }
        slot_map_ = std::make_unique<ClusterSlotMap>();
        slot_map_->Rebuild(nodes);
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

int MiniRedisServer::bindListen() const {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid bind address: " << config_.bind_addr << std::endl;
        close(listen_fd);
        return -1;
    }
    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    if (!setNonBlocking(listen_fd)) {
        perror("fcntl");
        close(listen_fd);
        return -1;
    }

    std::cout << "Listening on " << config_.bind_addr << ":" << config_.port << std::endl;
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
            std::cerr << "MySQL cluster discovery disabled: " << e.what() << std::endl;
        }

        if (mysql_ && !mysql_->registerNode(current_node_, 30)) {
            std::cerr << "Failed to register node in cluster table" << std::endl;
        }
    }
#else
    if (config_.enable_node_discovery) {
        std::cerr << "MySQL support is not available; dynamic node discovery disabled." << std::endl;
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
                    std::lock_guard<std::mutex> lock(slot_map_mutex_);
                    slot_map_->Rebuild(nodes);
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
                    std::lock_guard<std::mutex> lock(slot_map_mutex_);
                    slot_map_->MarkNodeHealthy(node);
                    continue;
                }

                bool ok = pingClusterNode(node, config_.requirepass);
                std::lock_guard<std::mutex> lock(slot_map_mutex_);
                if (ok) {
                    failed_heartbeats[node] = 0;
                    slot_map_->MarkNodeHealthy(node);
                } else {
                    int failures = ++failed_heartbeats[node];
                    if (failures >= config_.cluster_fail_threshold) {
                        slot_map_->MarkNodeFailed(node);
                    }
                }
            }
        }
    });
}

int MiniRedisServer::run() {
    running_ = true;
    g_running_signal_target = &running_;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (!configureCluster()) return 1;

    listen_fd_ = bindListen();
    if (listen_fd_ < 0) return 1;

    std::cout << "Mini-Redis server started (RESP ready)" << std::endl;
    if (!config_.requirepass.empty()) {
        std::cout << "AUTH enabled" << std::endl;
    }

    startStatsServer();
    startClusterDiscovery();

    PersistenceManager persistence(cache_, config_.snapshot_file, thread_pool_,
                                   config_.snapshot_interval_sec);
    persistence.start();

    CommandHandler command_handler(cache_, memory_pool_, config_, config_.cluster_mode,
                                   current_node_, slot_map_.get(), &slot_map_mutex_);
    command_handler.refreshRuntimeStats();

    scheduler_.schedule_task(acceptLoop(scheduler_, command_handler, config_, running_, listen_fd_));
    scheduler_thread_ = std::thread([this]() { scheduler_.start(); });

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

#ifdef HAVE_MYSQL
    if (config_.cluster_mode && mysql_) {
        mysql_->unregisterNode(current_node_);
    }
#endif
    if (cluster_refresh_thread_.joinable()) cluster_refresh_thread_.join();

    scheduler_.stop();
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
    if (stats_thread_.joinable()) stats_thread_.join();

    thread_pool_.stop();

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
