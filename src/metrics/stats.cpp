#include "miniredis/metrics/stats.hpp"
#include <algorithm>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace miniredis {
namespace {

uint64_t currentUnixMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

} // namespace

Stats& Stats::instance() {
    static Stats inst;
    return inst;
}

void Stats::recordCommand(const std::string& cmd, bool hit) {
    total_commands_++;
    if (cmd == "GET") {
        if (hit) get_hits_++;
        else get_misses_++;
    }
}

void Stats::recordSet() {
    total_commands_++;
}

void Stats::recordGetHit() {
    total_commands_++;
    get_hits_++;
}

void Stats::recordGetMiss() {
    total_commands_++;
    get_misses_++;
}

void Stats::recordConnectionOpen() {
    connected_clients_.fetch_add(1, std::memory_order_relaxed);
    total_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::recordConnectionClose() {
    size_t current = connected_clients_.load(std::memory_order_relaxed);
    while (current > 0 &&
           !connected_clients_.compare_exchange_weak(current, current - 1,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {}
}

void Stats::recordRejectedConnection() {
    rejected_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::recordCommandLatency(size_t latency_us) {
    latency_samples_.fetch_add(1, std::memory_order_relaxed);
    total_command_latency_us_.fetch_add(latency_us, std::memory_order_relaxed);

    size_t current_max = max_command_latency_us_.load(std::memory_order_relaxed);
    while (latency_us > current_max &&
           !max_command_latency_us_.compare_exchange_weak(current_max, latency_us,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {}
}

void Stats::recordCommandLatency(size_t latency_us, const std::vector<std::string>& command) {
    recordCommandLatency(latency_us);

    size_t threshold = slowlog_slower_than_us_.load(std::memory_order_relaxed);
    size_t max_len = slowlog_max_len_.load(std::memory_order_relaxed);
    if (threshold == 0 || max_len == 0 || latency_us < threshold) {
        return;
    }

    SlowLogEntry entry;
    entry.id = next_slowlog_id_.fetch_add(1, std::memory_order_relaxed);
    entry.unix_time = static_cast<int64_t>(std::time(nullptr));
    entry.duration_us = latency_us;
    entry.command = command;

    std::lock_guard<std::mutex> lock(slowlog_mutex_);
    slowlog_.push_front(std::move(entry));
    while (slowlog_.size() > max_len) {
        slowlog_.pop_back();
    }
}

void Stats::configureSlowLog(size_t slower_than_us, size_t max_len) {
    slowlog_slower_than_us_.store(slower_than_us, std::memory_order_relaxed);
    slowlog_max_len_.store(max_len, std::memory_order_relaxed);
    if (max_len == 0) {
        resetSlowLog();
        return;
    }
    std::lock_guard<std::mutex> lock(slowlog_mutex_);
    while (slowlog_.size() > max_len) {
        slowlog_.pop_back();
    }
}

std::vector<SlowLogEntry> Stats::slowLogEntries(size_t count) const {
    std::lock_guard<std::mutex> lock(slowlog_mutex_);
    size_t n = std::min(count, slowlog_.size());
    std::vector<SlowLogEntry> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        result.push_back(slowlog_[i]);
    }
    return result;
}

size_t Stats::slowLogLen() const {
    std::lock_guard<std::mutex> lock(slowlog_mutex_);
    return slowlog_.size();
}

void Stats::resetSlowLog() {
    std::lock_guard<std::mutex> lock(slowlog_mutex_);
    slowlog_.clear();
}

void Stats::setMemoryPoolUsed(size_t used_blocks, size_t free_blocks) {
    mem_pool_used_.store(used_blocks, std::memory_order_relaxed);//宽松模式，延迟可接收，追求速度！
    mem_pool_free_.store(free_blocks, std::memory_order_relaxed);
}

void Stats::setCacheMemory(size_t used_bytes, size_t max_bytes, size_t evicted_keys) {
    used_memory_bytes_.store(used_bytes, std::memory_order_relaxed);
    maxmemory_bytes_.store(max_bytes, std::memory_order_relaxed);
    evicted_keys_.store(evicted_keys, std::memory_order_relaxed);
}

void Stats::setResourceLimits(size_t max_request_bytes, size_t max_key_bytes,
                              size_t max_value_bytes, size_t max_pipeline_commands,
                              size_t client_output_buffer_limit, size_t max_clients) {
    max_request_bytes_.store(max_request_bytes, std::memory_order_relaxed);
    max_key_bytes_.store(max_key_bytes, std::memory_order_relaxed);
    max_value_bytes_.store(max_value_bytes, std::memory_order_relaxed);
    max_pipeline_commands_.store(max_pipeline_commands, std::memory_order_relaxed);
    client_output_buffer_limit_.store(client_output_buffer_limit, std::memory_order_relaxed);
    max_clients_.store(max_clients, std::memory_order_relaxed);
}

void Stats::setSnapshotRunning(bool running) {
    snapshot_running_.store(running, std::memory_order_relaxed);
}

void Stats::recordSnapshotResult(bool ok, size_t key_count, size_t duration_ms) {
    if (ok) {
        snapshot_last_success_unix_ms_.store(currentUnixMillis(), std::memory_order_relaxed);
        snapshot_last_key_count_.store(key_count, std::memory_order_relaxed);
        snapshot_last_duration_ms_.store(duration_ms, std::memory_order_relaxed);
        return;
    }
    snapshot_last_failure_unix_ms_.store(currentUnixMillis(), std::memory_order_relaxed);
    snapshot_failures_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::setAofRewriteRunning(bool running) {
    aof_rewrite_running_.store(running, std::memory_order_relaxed);
    if (running) setAofRewriteStatus("running");
}

void Stats::setAofRewriteBufferBytes(size_t bytes) {
    aof_rewrite_buffer_bytes_.store(bytes, std::memory_order_relaxed);
}

void Stats::recordAofRewriteResult(bool ok, size_t records, size_t duration_ms) {
    if (ok) {
        aof_last_rewrite_unix_ms_.store(currentUnixMillis(), std::memory_order_relaxed);
        aof_last_rewrite_records_.store(records, std::memory_order_relaxed);
        aof_last_rewrite_duration_ms_.store(duration_ms, std::memory_order_relaxed);
        setAofRewriteStatus("ok");
        return;
    }
    aof_last_rewrite_failure_unix_ms_.store(currentUnixMillis(), std::memory_order_relaxed);
    aof_rewrite_failures_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::setAofRewriteStatus(const std::string& status, const std::string& error) {
    std::lock_guard<std::mutex> lock(aof_rewrite_status_mutex_);
    aof_rewrite_last_status_ = status;
    aof_rewrite_last_error_ = error;
}

void Stats::setReplicationState(size_t configured_replicas, size_t connected_replicas,
                                uint64_t master_offset, uint64_t minimum_ack_offset,
                                uint64_t pending_offsets, uint64_t reconnects,
                                uint64_t errors, uint64_t backlog_misses) {
    replication_configured_replicas_.store(configured_replicas, std::memory_order_relaxed);
    replication_connected_replicas_.store(connected_replicas, std::memory_order_relaxed);
    replication_master_offset_.store(master_offset, std::memory_order_relaxed);
    replication_min_ack_offset_.store(minimum_ack_offset, std::memory_order_relaxed);
    replication_pending_offsets_.store(pending_offsets, std::memory_order_relaxed);
    replication_reconnects_.store(reconnects, std::memory_order_relaxed);
    replication_errors_.store(errors, std::memory_order_relaxed);
    replication_backlog_misses_.store(backlog_misses, std::memory_order_relaxed);
}

void Stats::setFailoverState(bool enabled, size_t group_size, size_t quorum,
                             size_t reachable_nodes, uint64_t current_epoch,
                             uint64_t leader_epoch, bool master_failed,
                             bool writes_allowed) {
    failover_enabled_.store(enabled, std::memory_order_relaxed);
    failover_group_size_.store(group_size, std::memory_order_relaxed);
    failover_quorum_.store(quorum, std::memory_order_relaxed);
    failover_reachable_nodes_.store(reachable_nodes, std::memory_order_relaxed);
    failover_current_epoch_.store(current_epoch, std::memory_order_relaxed);
    failover_leader_epoch_.store(leader_epoch, std::memory_order_relaxed);
    failover_master_failed_.store(master_failed, std::memory_order_relaxed);
    failover_writes_allowed_.store(writes_allowed, std::memory_order_relaxed);
}

void Stats::recordFailoverElection() {
    failover_elections_total_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::setNodeAddr(const std::string& addr) {
    std::lock_guard<std::mutex> lock(node_mutex_);
    node_addr_ = addr;
}

std::string Stats::getNodeAddr() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return node_addr_;
}

void Stats::setReady(bool ready) {
    ready_.store(ready, std::memory_order_relaxed);
}

bool Stats::ready() const {
    return ready_.load(std::memory_order_relaxed);
}

StatsSnapshot Stats::snapshot() const {
    StatsSnapshot snap;
    snap.node_addr = getNodeAddr();
    snap.total_commands = total_commands_.load(std::memory_order_relaxed);
    snap.get_hits = get_hits_.load(std::memory_order_relaxed);
    snap.get_misses = get_misses_.load(std::memory_order_relaxed);
    snap.key_count = key_count_.load(std::memory_order_relaxed);
    snap.cache_shards = cache_shards_.load(std::memory_order_relaxed);
    snap.mem_pool_used_blocks = mem_pool_used_.load(std::memory_order_relaxed);
    snap.mem_pool_free_blocks = mem_pool_free_.load(std::memory_order_relaxed);
    snap.used_memory_bytes = used_memory_bytes_.load(std::memory_order_relaxed);
    snap.maxmemory_bytes = maxmemory_bytes_.load(std::memory_order_relaxed);
    snap.evicted_keys = evicted_keys_.load(std::memory_order_relaxed);
    snap.connected_clients = connected_clients_.load(std::memory_order_relaxed);
    snap.total_connections = total_connections_.load(std::memory_order_relaxed);
    snap.rejected_connections = rejected_connections_.load(std::memory_order_relaxed);
    snap.latency_samples = latency_samples_.load(std::memory_order_relaxed);
    snap.max_command_latency_us = max_command_latency_us_.load(std::memory_order_relaxed);
    snap.slowlog_log_slower_than_us = slowlog_slower_than_us_.load(std::memory_order_relaxed);
    snap.slowlog_max_len = slowlog_max_len_.load(std::memory_order_relaxed);
    snap.slowlog_len = slowLogLen();
    snap.max_request_bytes = max_request_bytes_.load(std::memory_order_relaxed);
    snap.max_key_bytes = max_key_bytes_.load(std::memory_order_relaxed);
    snap.max_value_bytes = max_value_bytes_.load(std::memory_order_relaxed);
    snap.max_pipeline_commands = max_pipeline_commands_.load(std::memory_order_relaxed);
    snap.client_output_buffer_limit = client_output_buffer_limit_.load(std::memory_order_relaxed);
    snap.max_clients = max_clients_.load(std::memory_order_relaxed);
    snap.snapshot_running = snapshot_running_.load(std::memory_order_relaxed);
    snap.snapshot_last_success_unix_ms = snapshot_last_success_unix_ms_.load(std::memory_order_relaxed);
    snap.snapshot_last_failure_unix_ms = snapshot_last_failure_unix_ms_.load(std::memory_order_relaxed);
    snap.snapshot_last_duration_ms = snapshot_last_duration_ms_.load(std::memory_order_relaxed);
    snap.snapshot_last_key_count = snapshot_last_key_count_.load(std::memory_order_relaxed);
    snap.snapshot_failures = snapshot_failures_.load(std::memory_order_relaxed);
    snap.aof_rewrite_running = aof_rewrite_running_.load(std::memory_order_relaxed);
    snap.aof_rewrite_buffer_bytes = aof_rewrite_buffer_bytes_.load(std::memory_order_relaxed);
    snap.aof_last_rewrite_unix_ms = aof_last_rewrite_unix_ms_.load(std::memory_order_relaxed);
    snap.aof_last_rewrite_failure_unix_ms = aof_last_rewrite_failure_unix_ms_.load(std::memory_order_relaxed);
    snap.aof_last_rewrite_duration_ms = aof_last_rewrite_duration_ms_.load(std::memory_order_relaxed);
    snap.aof_last_rewrite_records = aof_last_rewrite_records_.load(std::memory_order_relaxed);
    snap.aof_rewrite_failures = aof_rewrite_failures_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(aof_rewrite_status_mutex_);
        snap.aof_rewrite_last_status = aof_rewrite_last_status_;
        snap.aof_rewrite_last_error = aof_rewrite_last_error_;
    }
    snap.replication_configured_replicas =
        replication_configured_replicas_.load(std::memory_order_relaxed);
    snap.replication_connected_replicas =
        replication_connected_replicas_.load(std::memory_order_relaxed);
    snap.replication_master_offset =
        replication_master_offset_.load(std::memory_order_relaxed);
    snap.replication_min_ack_offset =
        replication_min_ack_offset_.load(std::memory_order_relaxed);
    snap.replication_pending_offsets =
        replication_pending_offsets_.load(std::memory_order_relaxed);
    snap.replication_reconnects = replication_reconnects_.load(std::memory_order_relaxed);
    snap.replication_errors = replication_errors_.load(std::memory_order_relaxed);
    snap.replication_backlog_misses =
        replication_backlog_misses_.load(std::memory_order_relaxed);
    snap.failover_enabled = failover_enabled_.load(std::memory_order_relaxed);
    snap.failover_group_size = failover_group_size_.load(std::memory_order_relaxed);
    snap.failover_quorum = failover_quorum_.load(std::memory_order_relaxed);
    snap.failover_reachable_nodes = failover_reachable_nodes_.load(std::memory_order_relaxed);
    snap.failover_current_epoch = failover_current_epoch_.load(std::memory_order_relaxed);
    snap.failover_leader_epoch = failover_leader_epoch_.load(std::memory_order_relaxed);
    snap.failover_master_failed = failover_master_failed_.load(std::memory_order_relaxed);
    snap.failover_writes_allowed = failover_writes_allowed_.load(std::memory_order_relaxed);
    snap.failover_elections_total = failover_elections_total_.load(std::memory_order_relaxed);
    snap.ready = ready_.load(std::memory_order_relaxed);
    snap.io_threads = io_threads_.load(std::memory_order_relaxed);

    size_t total_gets = snap.get_hits + snap.get_misses;
    snap.hit_rate = total_gets == 0
                        ? 0.0
                        : static_cast<double>(snap.get_hits) /
                              static_cast<double>(total_gets);
    if (snap.latency_samples > 0) {
        snap.avg_command_latency_us =
            total_command_latency_us_.load(std::memory_order_relaxed) / snap.latency_samples;
    }
    snap.uptime_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_)
            .count());
    return snap;
}

std::string Stats::toJson() const {
    StatsSnapshot snap = snapshot();
    std::ostringstream oss;
    oss << "{";
    oss << "\"node_addr\":\"" << snap.node_addr << "\",";
    oss << "\"total_commands\":" << snap.total_commands << ",";
    oss << "\"get_hits\":" << snap.get_hits << ",";
    oss << "\"get_misses\":" << snap.get_misses << ",";
    oss << "\"hit_rate\":" << std::fixed << std::setprecision(4) << snap.hit_rate << ",";
    oss << "\"key_count\":" << snap.key_count << ",";
    oss << "\"cache_shards\":" << snap.cache_shards << ",";
    oss << "\"used_memory_bytes\":" << snap.used_memory_bytes << ",";
    oss << "\"maxmemory_bytes\":" << snap.maxmemory_bytes << ",";
    oss << "\"evicted_keys\":" << snap.evicted_keys << ",";
    oss << "\"mem_pool_used_blocks\":" << snap.mem_pool_used_blocks << ",";
    oss << "\"mem_pool_free_blocks\":" << snap.mem_pool_free_blocks << ",";
    oss << "\"connected_clients\":" << snap.connected_clients << ",";
    oss << "\"total_connections\":" << snap.total_connections << ",";
    oss << "\"rejected_connections\":" << snap.rejected_connections << ",";
    oss << "\"latency_samples\":" << snap.latency_samples << ",";
    oss << "\"avg_command_latency_us\":" << snap.avg_command_latency_us << ",";
    oss << "\"max_command_latency_us\":" << snap.max_command_latency_us << ",";
    oss << "\"slowlog_len\":" << snap.slowlog_len << ",";
    oss << "\"slowlog_log_slower_than_us\":" << snap.slowlog_log_slower_than_us << ",";
    oss << "\"slowlog_max_len\":" << snap.slowlog_max_len << ",";
    oss << "\"max_request_bytes\":" << snap.max_request_bytes << ",";
    oss << "\"max_key_bytes\":" << snap.max_key_bytes << ",";
    oss << "\"max_value_bytes\":" << snap.max_value_bytes << ",";
    oss << "\"max_pipeline_commands\":" << snap.max_pipeline_commands << ",";
    oss << "\"client_output_buffer_limit\":" << snap.client_output_buffer_limit << ",";
    oss << "\"max_clients\":" << snap.max_clients << ",";
    oss << "\"snapshot_running\":" << (snap.snapshot_running ? "true" : "false") << ",";
    oss << "\"snapshot_last_success_unix_ms\":" << snap.snapshot_last_success_unix_ms << ",";
    oss << "\"snapshot_last_failure_unix_ms\":" << snap.snapshot_last_failure_unix_ms << ",";
    oss << "\"snapshot_last_duration_ms\":" << snap.snapshot_last_duration_ms << ",";
    oss << "\"snapshot_last_key_count\":" << snap.snapshot_last_key_count << ",";
    oss << "\"snapshot_failures\":" << snap.snapshot_failures << ",";
    oss << "\"aof_rewrite_running\":" << (snap.aof_rewrite_running ? "true" : "false") << ",";
    oss << "\"aof_rewrite_buffer_bytes\":" << snap.aof_rewrite_buffer_bytes << ",";
    oss << "\"aof_last_rewrite_unix_ms\":" << snap.aof_last_rewrite_unix_ms << ",";
    oss << "\"aof_last_rewrite_failure_unix_ms\":" << snap.aof_last_rewrite_failure_unix_ms << ",";
    oss << "\"aof_last_rewrite_duration_ms\":" << snap.aof_last_rewrite_duration_ms << ",";
    oss << "\"aof_last_rewrite_records\":" << snap.aof_last_rewrite_records << ",";
    oss << "\"aof_rewrite_failures\":" << snap.aof_rewrite_failures << ",";
    oss << "\"aof_rewrite_last_status\":\"" << jsonEscape(snap.aof_rewrite_last_status) << "\",";
    oss << "\"aof_rewrite_last_error\":\"" << jsonEscape(snap.aof_rewrite_last_error) << "\",";
    oss << "\"replication_configured_replicas\":" << snap.replication_configured_replicas << ",";
    oss << "\"replication_connected_replicas\":" << snap.replication_connected_replicas << ",";
    oss << "\"replication_master_offset\":" << snap.replication_master_offset << ",";
    oss << "\"replication_min_ack_offset\":" << snap.replication_min_ack_offset << ",";
    oss << "\"replication_pending_offsets\":" << snap.replication_pending_offsets << ",";
    oss << "\"replication_reconnects\":" << snap.replication_reconnects << ",";
    oss << "\"replication_errors\":" << snap.replication_errors << ",";
    oss << "\"replication_backlog_misses\":" << snap.replication_backlog_misses << ",";
    oss << "\"failover_enabled\":" << (snap.failover_enabled ? "true" : "false") << ",";
    oss << "\"failover_group_size\":" << snap.failover_group_size << ",";
    oss << "\"failover_quorum\":" << snap.failover_quorum << ",";
    oss << "\"failover_reachable_nodes\":" << snap.failover_reachable_nodes << ",";
    oss << "\"failover_current_epoch\":" << snap.failover_current_epoch << ",";
    oss << "\"failover_leader_epoch\":" << snap.failover_leader_epoch << ",";
    oss << "\"failover_master_failed\":"
        << (snap.failover_master_failed ? "true" : "false") << ",";
    oss << "\"failover_writes_allowed\":"
        << (snap.failover_writes_allowed ? "true" : "false") << ",";
    oss << "\"failover_elections_total\":" << snap.failover_elections_total << ",";
    oss << "\"ready\":" << (snap.ready ? "true" : "false") << ",";
    oss << "\"io_threads\":" << snap.io_threads;
    oss << "}";
    return oss.str();
}

std::string Stats::toPrometheus() const {
    StatsSnapshot snap = snapshot();

    std::ostringstream oss;
    oss << "# HELP miniredis_total_commands Total processed commands.\n";
    oss << "# TYPE miniredis_total_commands counter\n";
    oss << "miniredis_total_commands " << snap.total_commands << "\n";
    oss << "# TYPE miniredis_get_hits counter\n";
    oss << "miniredis_get_hits " << snap.get_hits << "\n";
    oss << "# TYPE miniredis_get_misses counter\n";
    oss << "miniredis_get_misses " << snap.get_misses << "\n";
    oss << "# TYPE miniredis_hit_rate gauge\n";
    oss << "miniredis_hit_rate " << std::fixed << std::setprecision(4) << snap.hit_rate << "\n";
    oss << "# TYPE miniredis_key_count gauge\n";
    oss << "miniredis_key_count " << snap.key_count << "\n";
    oss << "# TYPE miniredis_cache_shards gauge\n";
    oss << "miniredis_cache_shards " << snap.cache_shards << "\n";
    oss << "# TYPE miniredis_used_memory_bytes gauge\n";
    oss << "miniredis_used_memory_bytes " << snap.used_memory_bytes << "\n";
    oss << "# TYPE miniredis_maxmemory_bytes gauge\n";
    oss << "miniredis_maxmemory_bytes " << snap.maxmemory_bytes << "\n";
    oss << "# TYPE miniredis_evicted_keys counter\n";
    oss << "miniredis_evicted_keys " << snap.evicted_keys << "\n";
    oss << "# TYPE miniredis_mem_pool_used_blocks gauge\n";
    oss << "miniredis_mem_pool_used_blocks " << snap.mem_pool_used_blocks << "\n";
    oss << "# TYPE miniredis_mem_pool_free_blocks gauge\n";
    oss << "miniredis_mem_pool_free_blocks " << snap.mem_pool_free_blocks << "\n";
    oss << "# TYPE miniredis_connected_clients gauge\n";
    oss << "miniredis_connected_clients " << snap.connected_clients << "\n";
    oss << "# TYPE miniredis_total_connections counter\n";
    oss << "miniredis_total_connections " << snap.total_connections << "\n";
    oss << "# TYPE miniredis_rejected_connections counter\n";
    oss << "miniredis_rejected_connections " << snap.rejected_connections << "\n";
    oss << "# TYPE miniredis_latency_samples counter\n";
    oss << "miniredis_latency_samples " << snap.latency_samples << "\n";
    oss << "# TYPE miniredis_avg_command_latency_us gauge\n";
    oss << "miniredis_avg_command_latency_us " << snap.avg_command_latency_us << "\n";
    oss << "# TYPE miniredis_max_command_latency_us gauge\n";
    oss << "miniredis_max_command_latency_us " << snap.max_command_latency_us << "\n";
    oss << "# TYPE miniredis_slowlog_len gauge\n";
    oss << "miniredis_slowlog_len " << snap.slowlog_len << "\n";
    oss << "# TYPE miniredis_slowlog_log_slower_than_us gauge\n";
    oss << "miniredis_slowlog_log_slower_than_us " << snap.slowlog_log_slower_than_us << "\n";
    oss << "# TYPE miniredis_slowlog_max_len gauge\n";
    oss << "miniredis_slowlog_max_len " << snap.slowlog_max_len << "\n";
    oss << "# TYPE miniredis_max_request_bytes gauge\n";
    oss << "miniredis_max_request_bytes " << snap.max_request_bytes << "\n";
    oss << "# TYPE miniredis_max_key_bytes gauge\n";
    oss << "miniredis_max_key_bytes " << snap.max_key_bytes << "\n";
    oss << "# TYPE miniredis_max_value_bytes gauge\n";
    oss << "miniredis_max_value_bytes " << snap.max_value_bytes << "\n";
    oss << "# TYPE miniredis_max_pipeline_commands gauge\n";
    oss << "miniredis_max_pipeline_commands " << snap.max_pipeline_commands << "\n";
    oss << "# TYPE miniredis_client_output_buffer_limit gauge\n";
    oss << "miniredis_client_output_buffer_limit " << snap.client_output_buffer_limit << "\n";
    oss << "# TYPE miniredis_max_clients gauge\n";
    oss << "miniredis_max_clients " << snap.max_clients << "\n";
    oss << "# TYPE miniredis_snapshot_running gauge\n";
    oss << "miniredis_snapshot_running " << (snap.snapshot_running ? 1 : 0) << "\n";
    oss << "# TYPE miniredis_snapshot_last_success_unix_ms gauge\n";
    oss << "miniredis_snapshot_last_success_unix_ms " << snap.snapshot_last_success_unix_ms << "\n";
    oss << "# TYPE miniredis_snapshot_last_failure_unix_ms gauge\n";
    oss << "miniredis_snapshot_last_failure_unix_ms " << snap.snapshot_last_failure_unix_ms << "\n";
    oss << "# TYPE miniredis_snapshot_last_duration_ms gauge\n";
    oss << "miniredis_snapshot_last_duration_ms " << snap.snapshot_last_duration_ms << "\n";
    oss << "# TYPE miniredis_snapshot_last_key_count gauge\n";
    oss << "miniredis_snapshot_last_key_count " << snap.snapshot_last_key_count << "\n";
    oss << "# TYPE miniredis_snapshot_failures counter\n";
    oss << "miniredis_snapshot_failures " << snap.snapshot_failures << "\n";
    oss << "# TYPE miniredis_aof_rewrite_running gauge\n";
    oss << "miniredis_aof_rewrite_running " << (snap.aof_rewrite_running ? 1 : 0) << "\n";
    oss << "# TYPE miniredis_aof_rewrite_buffer_bytes gauge\n";
    oss << "miniredis_aof_rewrite_buffer_bytes " << snap.aof_rewrite_buffer_bytes << "\n";
    oss << "# TYPE miniredis_aof_last_rewrite_unix_ms gauge\n";
    oss << "miniredis_aof_last_rewrite_unix_ms " << snap.aof_last_rewrite_unix_ms << "\n";
    oss << "# TYPE miniredis_aof_last_rewrite_failure_unix_ms gauge\n";
    oss << "miniredis_aof_last_rewrite_failure_unix_ms " << snap.aof_last_rewrite_failure_unix_ms << "\n";
    oss << "# TYPE miniredis_aof_last_rewrite_duration_ms gauge\n";
    oss << "miniredis_aof_last_rewrite_duration_ms " << snap.aof_last_rewrite_duration_ms << "\n";
    oss << "# TYPE miniredis_aof_last_rewrite_records gauge\n";
    oss << "miniredis_aof_last_rewrite_records " << snap.aof_last_rewrite_records << "\n";
    oss << "# TYPE miniredis_aof_rewrite_failures counter\n";
    oss << "miniredis_aof_rewrite_failures " << snap.aof_rewrite_failures << "\n";
    oss << "# TYPE miniredis_aof_rewrite_last_status_info gauge\n";
    oss << "miniredis_aof_rewrite_last_status_info{status=\""
        << jsonEscape(snap.aof_rewrite_last_status) << "\"} 1\n";
    oss << "# TYPE miniredis_replication_configured_replicas gauge\n";
    oss << "miniredis_replication_configured_replicas "
        << snap.replication_configured_replicas << "\n";
    oss << "# TYPE miniredis_replication_connected_replicas gauge\n";
    oss << "miniredis_replication_connected_replicas "
        << snap.replication_connected_replicas << "\n";
    oss << "# TYPE miniredis_replication_master_offset gauge\n";
    oss << "miniredis_replication_master_offset " << snap.replication_master_offset << "\n";
    oss << "# TYPE miniredis_replication_min_ack_offset gauge\n";
    oss << "miniredis_replication_min_ack_offset " << snap.replication_min_ack_offset << "\n";
    oss << "# TYPE miniredis_replication_pending_offsets gauge\n";
    oss << "miniredis_replication_pending_offsets " << snap.replication_pending_offsets << "\n";
    oss << "# TYPE miniredis_replication_reconnects counter\n";
    oss << "miniredis_replication_reconnects " << snap.replication_reconnects << "\n";
    oss << "# TYPE miniredis_replication_errors counter\n";
    oss << "miniredis_replication_errors " << snap.replication_errors << "\n";
    oss << "# TYPE miniredis_replication_backlog_misses counter\n";
    oss << "miniredis_replication_backlog_misses "
        << snap.replication_backlog_misses << "\n";
    oss << "# TYPE miniredis_failover_enabled gauge\n";
    oss << "miniredis_failover_enabled " << (snap.failover_enabled ? 1 : 0) << "\n";
    oss << "# TYPE miniredis_failover_group_size gauge\n";
    oss << "miniredis_failover_group_size " << snap.failover_group_size << "\n";
    oss << "# TYPE miniredis_failover_quorum gauge\n";
    oss << "miniredis_failover_quorum " << snap.failover_quorum << "\n";
    oss << "# TYPE miniredis_failover_reachable_nodes gauge\n";
    oss << "miniredis_failover_reachable_nodes " << snap.failover_reachable_nodes << "\n";
    oss << "# TYPE miniredis_failover_current_epoch gauge\n";
    oss << "miniredis_failover_current_epoch " << snap.failover_current_epoch << "\n";
    oss << "# TYPE miniredis_failover_leader_epoch gauge\n";
    oss << "miniredis_failover_leader_epoch " << snap.failover_leader_epoch << "\n";
    oss << "# TYPE miniredis_failover_master_failed gauge\n";
    oss << "miniredis_failover_master_failed " << (snap.failover_master_failed ? 1 : 0)
        << "\n";
    oss << "# TYPE miniredis_failover_writes_allowed gauge\n";
    oss << "miniredis_failover_writes_allowed " << (snap.failover_writes_allowed ? 1 : 0)
        << "\n";
    oss << "# TYPE miniredis_failover_elections_total counter\n";
    oss << "miniredis_failover_elections_total " << snap.failover_elections_total << "\n";
    oss << "# TYPE miniredis_ready gauge\n";
    oss << "miniredis_ready " << (snap.ready ? 1 : 0) << "\n";
    oss << "# TYPE miniredis_io_threads gauge\n";
    oss << "miniredis_io_threads " << snap.io_threads << "\n";
    return oss.str();
}

}
