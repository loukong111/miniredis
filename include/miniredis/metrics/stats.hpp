#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <cstddef>
#include <vector>

namespace miniredis {

struct StatsSnapshot {
    size_t total_commands = 0;
    size_t get_hits = 0;
    size_t get_misses = 0;
    double hit_rate = 0.0;
    size_t key_count = 0;
    size_t cache_shards = 0;
    size_t mem_pool_used_blocks = 0;
    size_t mem_pool_free_blocks = 0;
    size_t used_memory_bytes = 0;
    size_t maxmemory_bytes = 0;
    size_t evicted_keys = 0;
    size_t connected_clients = 0;
    size_t total_connections = 0;
    size_t rejected_connections = 0;
    size_t latency_samples = 0;
    size_t avg_command_latency_us = 0;
    size_t max_command_latency_us = 0;
    size_t slowlog_len = 0;
    size_t slowlog_log_slower_than_us = 0;
    size_t slowlog_max_len = 0;
    size_t max_request_bytes = 0;
    size_t max_key_bytes = 0;
    size_t max_value_bytes = 0;
    size_t max_pipeline_commands = 0;
    size_t client_output_buffer_limit = 0;
    size_t max_clients = 0;
    bool snapshot_running = false;
    uint64_t snapshot_last_success_unix_ms = 0;
    uint64_t snapshot_last_failure_unix_ms = 0;
    size_t snapshot_last_duration_ms = 0;
    size_t snapshot_last_key_count = 0;
    size_t snapshot_failures = 0;
    bool aof_rewrite_running = false;
    size_t aof_rewrite_buffer_bytes = 0;
    uint64_t aof_last_rewrite_unix_ms = 0;
    uint64_t aof_last_rewrite_failure_unix_ms = 0;
    size_t aof_last_rewrite_duration_ms = 0;
    size_t aof_last_rewrite_records = 0;
    size_t aof_rewrite_failures = 0;
    std::string aof_rewrite_last_status;
    std::string aof_rewrite_last_error;
    size_t replication_configured_replicas = 0;
    size_t replication_connected_replicas = 0;
    uint64_t replication_master_offset = 0;
    uint64_t replication_min_ack_offset = 0;
    uint64_t replication_pending_offsets = 0;
    uint64_t replication_reconnects = 0;
    uint64_t replication_errors = 0;
    uint64_t replication_backlog_misses = 0;
    bool failover_enabled = false;
    size_t failover_group_size = 0;
    size_t failover_quorum = 0;
    size_t failover_reachable_nodes = 0;
    uint64_t failover_current_epoch = 0;
    uint64_t failover_leader_epoch = 0;
    bool failover_master_failed = false;
    bool failover_writes_allowed = false;
    uint64_t failover_elections_total = 0;
    uint64_t uptime_seconds = 0;
    bool ready = false;
    size_t io_threads = 0;
    std::string node_addr;
};

struct SlowLogEntry {
    uint64_t id = 0;
    int64_t unix_time = 0;
    size_t duration_us = 0;
    std::vector<std::string> command;
};

class Stats {
public:
    static Stats& instance();

    void recordCommand(const std::string& cmd, bool hit = false);
    void recordSet();
    void recordGetHit();
    void recordGetMiss();
    void recordConnectionOpen();
    void recordConnectionClose();
    void recordRejectedConnection();
    void recordCommandLatency(size_t latency_us);
    void recordCommandLatency(size_t latency_us, const std::vector<std::string>& command);

    void setNodeAddr(const std::string& addr);
    std::string getNodeAddr() const;
    void setReady(bool ready);
    bool ready() const;
    void setIoThreads(size_t count) { io_threads_.store(count, std::memory_order_relaxed); }
    void configureSlowLog(size_t slower_than_us, size_t max_len);
    std::vector<SlowLogEntry> slowLogEntries(size_t count) const;
    size_t slowLogLen() const;
    void resetSlowLog();

    void setKeyCount(size_t count) { key_count_.store(count, std::memory_order_relaxed); }
    void setCacheShards(size_t count) { cache_shards_.store(count, std::memory_order_relaxed); }
    void setMemoryPoolUsed(size_t used_blocks, size_t free_blocks);
    void setCacheMemory(size_t used_bytes, size_t max_bytes, size_t evicted_keys);
    void setResourceLimits(size_t max_request_bytes, size_t max_key_bytes,
                           size_t max_value_bytes, size_t max_pipeline_commands,
                           size_t client_output_buffer_limit, size_t max_clients);
    void setSnapshotRunning(bool running);
    void recordSnapshotResult(bool ok, size_t key_count, size_t duration_ms);
    void setAofRewriteRunning(bool running);
    void setAofRewriteBufferBytes(size_t bytes);
    void recordAofRewriteResult(bool ok, size_t records, size_t duration_ms);
    void setAofRewriteStatus(const std::string& status, const std::string& error = {});
    void setReplicationState(size_t configured_replicas, size_t connected_replicas,
                             uint64_t master_offset, uint64_t minimum_ack_offset,
                             uint64_t pending_offsets, uint64_t reconnects,
                             uint64_t errors, uint64_t backlog_misses);
    void setFailoverState(bool enabled, size_t group_size, size_t quorum,
                          size_t reachable_nodes, uint64_t current_epoch,
                          uint64_t leader_epoch, bool master_failed,
                          bool writes_allowed);
    void recordFailoverElection();
    size_t totalCommands() const { return total_commands_.load(std::memory_order_relaxed); }
    size_t connectedClients() const { return connected_clients_.load(std::memory_order_relaxed); }

    StatsSnapshot snapshot() const;
    std::string toJson() const;
    std::string toPrometheus() const;

private:
    Stats() = default;//单例，私有化

    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::atomic<size_t> total_commands_{0};
    std::atomic<size_t> get_hits_{0};
    std::atomic<size_t> get_misses_{0};
    std::atomic<size_t> key_count_{0};
    std::atomic<size_t> cache_shards_{0};
    std::atomic<size_t> mem_pool_used_{0};
    std::atomic<size_t> mem_pool_free_{0};
    std::atomic<size_t> used_memory_bytes_{0};
    std::atomic<size_t> maxmemory_bytes_{0};
    std::atomic<size_t> evicted_keys_{0};
    std::atomic<size_t> connected_clients_{0};
    std::atomic<size_t> total_connections_{0};
    std::atomic<size_t> rejected_connections_{0};
    std::atomic<size_t> latency_samples_{0};
    std::atomic<size_t> total_command_latency_us_{0};
    std::atomic<size_t> max_command_latency_us_{0};
    std::atomic<size_t> slowlog_slower_than_us_{10000};
    std::atomic<size_t> slowlog_max_len_{128};
    std::atomic<size_t> max_request_bytes_{0};
    std::atomic<size_t> max_key_bytes_{0};
    std::atomic<size_t> max_value_bytes_{0};
    std::atomic<size_t> max_pipeline_commands_{0};
    std::atomic<size_t> client_output_buffer_limit_{0};
    std::atomic<size_t> max_clients_{0};
    std::atomic<bool> snapshot_running_{false};
    std::atomic<uint64_t> snapshot_last_success_unix_ms_{0};
    std::atomic<uint64_t> snapshot_last_failure_unix_ms_{0};
    std::atomic<size_t> snapshot_last_duration_ms_{0};
    std::atomic<size_t> snapshot_last_key_count_{0};
    std::atomic<size_t> snapshot_failures_{0};
    std::atomic<bool> aof_rewrite_running_{false};
    std::atomic<size_t> aof_rewrite_buffer_bytes_{0};
    std::atomic<uint64_t> aof_last_rewrite_unix_ms_{0};
    std::atomic<uint64_t> aof_last_rewrite_failure_unix_ms_{0};
    std::atomic<size_t> aof_last_rewrite_duration_ms_{0};
    std::atomic<size_t> aof_last_rewrite_records_{0};
    std::atomic<size_t> aof_rewrite_failures_{0};
    std::string aof_rewrite_last_status_;
    std::string aof_rewrite_last_error_;
    mutable std::mutex aof_rewrite_status_mutex_;
    std::atomic<size_t> replication_configured_replicas_{0};
    std::atomic<size_t> replication_connected_replicas_{0};
    std::atomic<uint64_t> replication_master_offset_{0};
    std::atomic<uint64_t> replication_min_ack_offset_{0};
    std::atomic<uint64_t> replication_pending_offsets_{0};
    std::atomic<uint64_t> replication_reconnects_{0};
    std::atomic<uint64_t> replication_errors_{0};
    std::atomic<uint64_t> replication_backlog_misses_{0};
    std::atomic<bool> failover_enabled_{false};
    std::atomic<size_t> failover_group_size_{0};
    std::atomic<size_t> failover_quorum_{0};
    std::atomic<size_t> failover_reachable_nodes_{0};
    std::atomic<uint64_t> failover_current_epoch_{0};
    std::atomic<uint64_t> failover_leader_epoch_{0};
    std::atomic<bool> failover_master_failed_{false};
    std::atomic<bool> failover_writes_allowed_{false};
    std::atomic<uint64_t> failover_elections_total_{0};
    std::atomic<bool> ready_{false};
    std::atomic<size_t> io_threads_{0};
    std::atomic<uint64_t> next_slowlog_id_{0};
    std::deque<SlowLogEntry> slowlog_;
    mutable std::mutex slowlog_mutex_;
    std::string node_addr_;
    mutable std::mutex node_mutex_;
};

}
