#pragma once

#include "miniredis/cluster/slot_map.hpp"
#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/core/thread_pool.hpp"
#include "miniredis/net/scheduler.hpp"
#include "miniredis/server/config.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef HAVE_MYSQL
#include "miniredis/cluster/mysql_client.hpp"
#endif

namespace miniredis {

class MiniRedisServer {
public:
    explicit MiniRedisServer(AppConfig config);
    ~MiniRedisServer();

    MiniRedisServer(const MiniRedisServer&) = delete;
    MiniRedisServer& operator=(const MiniRedisServer&) = delete;

    int run();

private:
    bool configureCluster();
    int bindListen() const;
    void startStatsServer();
    void startClusterDiscovery();
    void stop();

    AppConfig config_;
    FixedMemoryPool memory_pool_;
    DynamicThreadPool thread_pool_;
    CacheStore cache_;
    Scheduler scheduler_;

    std::atomic<bool> running_;
    std::atomic<bool> stats_running_;
    int listen_fd_;

    std::string current_node_;
    std::unique_ptr<ClusterSlotMap> slot_map_;
#ifdef HAVE_MYSQL
    std::unique_ptr<MySQLClient> mysql_;
#endif
    std::mutex slot_map_mutex_;

    std::thread stats_thread_;
    std::thread scheduler_thread_;
    std::thread cluster_refresh_thread_;
};

} // namespace miniredis
