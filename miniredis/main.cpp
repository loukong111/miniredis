#include "http_stats.hpp" 
#include "scheduler.hpp"
#include "cache_store.hpp"
#include "memory_pool.hpp"
#include "thread_pool.hpp"
#include "resp_parser.hpp"
#include "mysql_client.hpp"
#include "persistence_manager.hpp"
#include "consistent_hash.hpp"
#include "stats.hpp"
#include <getopt.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <coroutine>
#include <vector>
#include <csignal>
#include <atomic>
#include <memory>
#include <mutex>

using namespace miniredis;

std::atomic<bool> g_running(true);
std::mutex g_hash_ring_mutex;           // 保护哈希环的全局锁

// 全局集群配置
static bool g_cluster_mode = false;
static std::string g_current_node;
static std::unique_ptr<ConsistentHash> g_hash_ring;

// 从 "ip:port" 字符串中提取端口，并绑定监听
static int bind_listen_port_from_addr(const std::string& addr) {
    size_t colon = addr.find(':');
    if (colon == std::string::npos) {
        std::cerr << "Invalid node address format (expected ip:port): " << addr << std::endl;
        return -1;
    }
    std::string port_str = addr.substr(colon + 1);
    int port = std::stoi(port_str);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr_in{};
    addr_in.sin_family = AF_INET;
    addr_in.sin_port = htons(port);
    addr_in.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr*)&addr_in, sizeof(addr_in)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);
    std::cout << "Listening on port " << port << std::endl;
    return listen_fd;
}

// 命令处理（支持集群 MOVED 重定向）
static std::string handleCommand(const RespValue& cmd, CacheStore& cache) {
    if (cmd.type != RespType::ARRAY || cmd.array.empty())
        return RespWriter::error("invalid command format");
    const RespValue& first = cmd.array[0];
    if (first.type != RespType::BULK_STRING && first.type != RespType::SIMPLE_STRING)
        return RespWriter::error("command name must be string");
    std::string cmdName = first.str;
    for (auto& c : cmdName) c = toupper(c);

    // 需要路由的命令（依赖 key）
    bool needs_route = (cmdName == "GET" || cmdName == "SET" || cmdName == "DEL" || cmdName == "EXISTS");
    if (needs_route && g_cluster_mode) {
        if (cmd.array.size() < 2)
            return RespWriter::error("wrong number of arguments for '" + cmdName + "'");
        const RespValue& keyArg = cmd.array[1];
        if (keyArg.type != RespType::BULK_STRING)
            return RespWriter::error("key must be bulk string");
        std::string key = keyArg.str;
        if (!g_hash_ring) {
            return RespWriter::error("cluster ring not initialized");
        }
        // 加锁读取哈希环
        std::string target;
        {
            std::lock_guard<std::mutex> lock(g_hash_ring_mutex);
            target = g_hash_ring->GetNode(key);
        }
        if (target != g_current_node) {
            return "-MOVED 0 " + target + "\r\n";
        }
    }

    // 正常命令处理
    if (cmdName == "GET") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'get'");
        const RespValue& keyArg = cmd.array[1];
        if (keyArg.type != RespType::BULK_STRING) return RespWriter::error("key must be bulk string");
        auto val = cache.get(keyArg.str);
        if (val) {
            Stats::instance().recordGetHit();
            return RespWriter::bulkString(*val);
        } else {
            Stats::instance().recordGetMiss();
            return RespWriter::nullBulkString();
        }
    }
    else if (cmdName == "SET") {
        if (cmd.array.size() != 3) return RespWriter::error("wrong number of arguments for 'set'");
        const RespValue& keyArg = cmd.array[1];
        const RespValue& valArg = cmd.array[2];
        if (keyArg.type != RespType::BULK_STRING || valArg.type != RespType::BULK_STRING)
            return RespWriter::error("key and value must be bulk strings");
        cache.set(keyArg.str, valArg.str);
        Stats::instance().recordSet();
        return RespWriter::simpleString("OK");
    }
    else if (cmdName == "DEL") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'del'");
        int deleted = 0;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            if (cmd.array[i].type == RespType::BULK_STRING && cache.del(cmd.array[i].str))
                ++deleted;
        }
        Stats::instance().recordCommand(cmdName);
        return RespWriter::integer(deleted);
    }
    else if (cmdName == "EXISTS") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'exists'");
        int count = 0;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            if (cmd.array[i].type == RespType::BULK_STRING && cache.exists(cmd.array[i].str))
                ++count;
        }
        Stats::instance().recordCommand(cmdName);
        return RespWriter::integer(count);
    }
    else if (cmdName == "PING") {
        if (cmd.array.size() == 1) return RespWriter::simpleString("PONG");
        else if (cmd.array.size() == 2 && cmd.array[1].type == RespType::BULK_STRING)
            return RespWriter::bulkString(cmd.array[1].str);
        else return RespWriter::error("invalid PING arguments");
    }
    else if (cmdName == "COMMAND") {
        return RespWriter::array({});
    }
    else {
        return RespWriter::error("unknown command '" + cmdName + "'");
    }
}

Task handle_client(Scheduler& scheduler, CacheStore& cache, int client_fd) {
    RespDecoder decoder;
    char buffer[4096];
    std::string outbuf;
    bool writing = false;

    while (true) {
        if (!writing) {
            co_await scheduler.await_io(client_fd, EPOLLIN);
        }

        if (!writing) {
            ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                if (n == 0) {
                    // normal close
                } else if (errno != EAGAIN) {
                    perror("read error");
                }
                scheduler.del_fd(client_fd);
                close(client_fd);
                break;
            }
            buffer[n] = '\0';
            decoder.feed(std::string_view(buffer, n));

            while (true) {
                auto optCmd = decoder.parse();
                if (!optCmd) break;
                std::string response = handleCommand(*optCmd, cache);
                outbuf += response;
            }
        }

        if (!outbuf.empty()) {
            ssize_t n = write(client_fd, outbuf.data(), outbuf.size());
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    writing = true;
                    co_await scheduler.await_io(client_fd, EPOLLOUT);
                    continue;
                } else {
                    perror("write error");
                    scheduler.del_fd(client_fd);
                    close(client_fd);
                    break;
                }
            }
            if (n == (ssize_t)outbuf.size()) {
                outbuf.clear();
                writing = false;
            } else {
                outbuf.erase(0, n);
                writing = true;
                co_await scheduler.await_io(client_fd, EPOLLOUT);
            }
        } else {
            writing = false;
        }
    }
    co_return;
}

Task accept_loop(Scheduler& scheduler, CacheStore& cache, int listen_fd) {
    std::vector<Task> client_tasks;
    while (true) {
        co_await scheduler.await_io(listen_fd, EPOLLIN);
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            client_tasks.push_back(handle_client(scheduler, cache, client_fd));
        }
        for (auto it = client_tasks.begin(); it != client_tasks.end(); ) {
            if (it->done()) it = client_tasks.erase(it);
            else ++it;
        }
    }
}

void signalHandler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 解析命令行参数
    static struct option long_options[] = {
        {"cluster",   no_argument,       0, 'c'},
        {"node-addr", required_argument, 0, 'a'},
        {"nodes",     required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };
    int opt;
    std::string node_addr, nodes_str;
    while ((opt = getopt_long(argc, argv, "ca:n:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                g_cluster_mode = true;
                break;
            case 'a':
                node_addr = optarg;
                break;
            case 'n':
                nodes_str = optarg;
                break;
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [--cluster] --node-addr ip:port --nodes ip1:port1,ip2:port2,..." << std::endl;
                return 1;
        }
    }

    if (g_cluster_mode) {
        if (node_addr.empty() || nodes_str.empty()) {
            std::cerr << "Cluster mode requires --node-addr and --nodes" << std::endl;
            return 1;
        }
        g_current_node = node_addr;
        g_hash_ring = std::make_unique<ConsistentHash>(150);
        size_t start = 0, end = 0;
        while ((end = nodes_str.find(',', start)) != std::string::npos) {
            std::string node = nodes_str.substr(start, end - start);
            g_hash_ring->AddNode(node);
            start = end + 1;
        }
        g_hash_ring->AddNode(nodes_str.substr(start));
    }

    FixedMemoryPool memory_pool(1024);
    DynamicThreadPool thread_pool(4, 16, 5);
    CacheStore cache(memory_pool);
    Scheduler scheduler;

    int listen_fd;
    if (g_cluster_mode) {
        listen_fd = bind_listen_port_from_addr(g_current_node);
        if (listen_fd < 0) return 1;
    } else {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) { perror("socket"); return 1; }
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(6366);   // 用户指定的端口
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
        if (listen(listen_fd, 128) < 0) { perror("listen"); return 1; }
        fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);
        std::cout << "Standalone mode listening on port 6366" << std::endl;
    }

    std::cout << "Mini-Redis server started (RESP ready)" << std::endl;

    // HTTP 统计线程
    std::atomic<bool> stats_running(true);
    std::thread stats_thread([&stats_running]() {
        start_stats_http_server(8080, stats_running);
    });
    stats_thread.detach();

#ifdef HAVE_MYSQL
    MySQLClient mysql("127.0.0.1", "miniredis", "198407", "miniredis", 3306);
    // 持久化管理器
    PersistenceManager persistence(cache, mysql, thread_pool);
    persistence.start();

    // 动态节点发现（仅在集群模式下）
    if (g_cluster_mode) {
        // 注册当前节点到集群表
        if (!mysql.registerNode(g_current_node, 30)) {
            std::cerr << "Failed to register node in cluster table" << std::endl;
        }
        // 启动后台刷新线程
        std::thread cluster_refresh_thread([&mysql]() {
            while (g_running && g_cluster_mode) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                auto nodes = mysql.getActiveNodes(30);
                if (!nodes.empty()) {
                    std::lock_guard<std::mutex> lock(g_hash_ring_mutex);
                    g_hash_ring->clear();
                    for (const auto& node : nodes) {
                        g_hash_ring->AddNode(node);
                    }
                }
            }
        });
        cluster_refresh_thread.detach();
    }
#endif

    auto acceptor = accept_loop(scheduler, cache, listen_fd);
    acceptor.resume();

    std::thread scheduler_thread([&scheduler]() { scheduler.start(); });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理
    if (g_cluster_mode) {
#ifdef HAVE_MYSQL
        MySQLClient mysql("127.0.0.1", "miniredis", "198407", "miniredis", 3306);
        mysql.unregisterNode(g_current_node);
#endif
    }
#ifdef HAVE_MYSQL
    persistence.stop();
#endif
    scheduler.stop();
    if (scheduler_thread.joinable()) scheduler_thread.join();
    thread_pool.stop();
    close(listen_fd);
    return 0;
}