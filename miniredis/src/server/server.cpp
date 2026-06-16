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
#include <sstream>
#include <sys/socket.h>
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
    std::vector<Task> client_tasks;
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
            Stats::instance().recordConnectionOpen();
            client_tasks.push_back(handleClient(scheduler, command_handler, config, client_fd));
        }

        for (auto it = client_tasks.begin(); it != client_tasks.end(); ) {
            if (it->done()) it = client_tasks.erase(it);
            else ++it;
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

        hash_ring_ = std::make_unique<ConsistentHash>(150);
        size_t start = 0;
        size_t end = 0;
        while ((end = config_.nodes_str.find(',', start)) != std::string::npos) {
            std::string node = config_.nodes_str.substr(start, end - start);
            if (!node.empty()) hash_ring_->AddNode(node);
            start = end + 1;
        }
        std::string last = config_.nodes_str.substr(start);
        if (!last.empty()) hash_ring_->AddNode(last);
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
    if (!config_.cluster_mode || !config_.enable_node_discovery) return;

#ifdef HAVE_MYSQL
    try {
        mysql_ = std::make_unique<MySQLClient>(config_.mysql_host, config_.mysql_user,
                                               config_.mysql_pass, config_.mysql_db,
                                               static_cast<unsigned int>(config_.mysql_port));
    } catch (const std::exception& e) {
        std::cerr << "MySQL cluster discovery disabled: " << e.what() << std::endl;
        return;
    }

    if (!mysql_->registerNode(current_node_, 30)) {
        std::cerr << "Failed to register node in cluster table" << std::endl;
    }

    cluster_refresh_thread_ = std::thread([this]() {
        while (running_.load() && config_.cluster_mode) {
            for (int i = 0; i < 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!running_.load()) break;

            auto nodes = mysql_->getActiveNodes(30);
            if (!nodes.empty()) {
                std::lock_guard<std::mutex> lock(hash_ring_mutex_);
                hash_ring_->clear();
                for (const auto& node : nodes) {
                    hash_ring_->AddNode(node);
                }
            }
        }
    });
#else
    std::cerr << "MySQL support is not available; dynamic node discovery disabled." << std::endl;
#endif
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
                                   current_node_, hash_ring_.get(), &hash_ring_mutex_);
    command_handler.refreshRuntimeStats();

    auto acceptor = acceptLoop(scheduler_, command_handler, config_, running_, listen_fd_);
    acceptor.resume();
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
