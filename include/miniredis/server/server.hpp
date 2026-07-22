#pragma once

#include "miniredis/cluster/slot_map.hpp"
#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/core/thread_pool.hpp"
#include "miniredis/net/scheduler.hpp"
#include "miniredis/persistence/append_only_file.hpp"
#include "miniredis/server/config.hpp"
#include "miniredis/server/failover_coordinator.hpp"
#include "miniredis/server/replication_backlog.hpp"
#include "miniredis/server/replication_dispatcher.hpp"
#include "miniredis/server/replication_state.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef HAVE_MYSQL
#include "miniredis/cluster/mysql_client.hpp"
#endif

namespace miniredis {

struct RespValue;

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
    bool configureFailover();
    void startFailoverMonitor();
    std::string handleFailoverCommand(const RespValue& command);
    std::string failoverInfo() const;
    void updateFailoverStats(size_t reachable_nodes) const;
    std::string activeMaster() const;
    void promoteAfterElection(uint64_t epoch, const std::string& old_master);
    void adoptFailoverLeader(uint64_t epoch, const std::string& leader,
                             const std::string& old_master);
    size_t transferClusterSlots(const std::string& old_master,
                                const std::string& new_master, uint64_t epoch);
    void startReplicationSync();
    bool syncFromMaster();
    bool tryPartialSyncFromMaster(const std::string& host, int port,
                                  const std::string& last_replid, uint64_t last_offset);
    bool fullSyncFromMaster(const std::string& host, int port);
    bool applyReplicationCommand(const std::vector<std::string>& command,
                                 const std::string& replid, uint64_t offset);
    std::string replicationStateFile() const;
    std::string legacyReplicationOffsetFile() const;
    ReplicationState loadReplicationState() const;
    bool saveReplicationState(uint64_t offset) const;
    void saveClusterConfigIfNeeded() const;
    void stop();

    AppConfig config_;
    FixedMemoryPool memory_pool_;
    DynamicThreadPool thread_pool_;
    CacheStore cache_;
    Scheduler accept_scheduler_;
    std::vector<std::unique_ptr<Scheduler>> io_schedulers_;
    std::atomic<size_t> next_io_scheduler_;

    std::atomic<bool> running_;
    std::atomic<bool> stats_running_;
    int listen_fd_;

    std::string current_node_;
    std::unique_ptr<AppendOnlyFile> aof_;
    std::string local_replid_;
    ReplicationBacklog replication_backlog_;
    std::atomic<uint64_t> replication_offset_;
    std::string upstream_replid_;
    bool replication_state_loaded_ = false;
    std::unique_ptr<ReplicationDispatcher> replication_dispatcher_;
    std::mutex replication_apply_mutex_;
    std::atomic<bool> replica_following_{false};
    std::atomic<bool> primary_state_{true};
    std::atomic<bool> writes_allowed_{true};
    std::atomic<bool> master_failed_{false};
    std::vector<std::string> failover_nodes_;
    std::unique_ptr<FailoverCoordinator> failover_coordinator_;
    std::mutex failover_transition_mutex_;
    mutable std::mutex failover_runtime_mutex_;
    std::string active_master_;
    std::unique_ptr<ClusterSlotMap> slot_map_;
#ifdef HAVE_MYSQL
    std::unique_ptr<MySQLClient> mysql_;
#endif
    std::mutex slot_map_mutex_;
    mutable std::mutex cluster_config_save_mutex_;

    std::thread stats_thread_;
    std::thread accept_thread_;
    std::vector<std::thread> io_threads_;
    std::thread cluster_refresh_thread_;
    std::thread replication_sync_thread_;
    std::thread failover_thread_;
};

} // namespace miniredis
