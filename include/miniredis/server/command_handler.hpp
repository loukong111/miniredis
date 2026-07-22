#pragma once

#include "miniredis/cluster/slot_map.hpp"
#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/net/resp_parser.hpp"
#include "miniredis/persistence/append_only_file.hpp"
#include "miniredis/server/config.hpp"
#include "miniredis/server/replication_backlog.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace miniredis {

class ReplicationDispatcher;

struct CommandSession {
    bool authenticated = false;
    bool asking = false;
    AclRole role = AclRole::Admin;
    std::string username;
    bool command_allowlist_enabled = false;
    std::vector<std::string> allowed_commands;
    std::vector<std::string> denied_commands;
    bool all_keys = true;
    std::vector<std::string> key_prefixes;
};

class CommandHandler {
public:
    CommandHandler(CacheStore& cache, FixedMemoryPool& memory_pool, const AppConfig& config,
                   bool cluster_mode, std::string current_node,
                   ClusterSlotMap* slot_map, std::mutex* slot_map_mutex,
                   AppendOnlyFile* aof = nullptr,
                   std::function<void()> cluster_change_callback = {},
                   ReplicationBacklog* replication_backlog = nullptr,
                   std::atomic<uint64_t>* replication_offset = nullptr,
                   std::function<void(uint64_t)> replication_offset_callback = {},
                   ReplicationDispatcher* replication_dispatcher = nullptr,
                   std::mutex* replication_apply_mutex = nullptr,
                   std::function<void()> replica_promoted_callback = {},
                   const std::string* local_replid = nullptr,
                   std::string* upstream_replid = nullptr,
                   std::atomic<bool>* primary_state = nullptr,
                   std::atomic<bool>* writes_allowed = nullptr,
                   std::function<std::string()> active_master_callback = {},
                   std::function<std::string(const RespValue&)> failover_command_callback = {},
                   std::function<std::string()> failover_info_callback = {});

    std::string handle(const RespValue& cmd, CommandSession& session);
    std::string handle(const RespValue& cmd, bool& authenticated);
    void refreshRuntimeStats() const;

private:
    bool isReplica() const;
    bool authRequired() const;
    bool authenticate(const RespValue& cmd, CommandSession& session) const;
    bool isAllowed(const std::string& cmd_name, const RespValue& cmd,
                   const CommandSession& session) const;
    std::string validateKey(const RespValue& key) const;
    std::string validateValue(const RespValue& value) const;
    std::string handleAclCommand(const RespValue& cmd, const CommandSession& session) const;
    std::string routeIfNeeded(const std::string& cmd_name, const RespValue& cmd,
                              bool asking) const;
    std::string handleInfoCommand(const RespValue& cmd) const;
    std::string handleSlowLogCommand(const RespValue& cmd) const;
    std::string handleClusterCommand(const RespValue& cmd);
    std::string handleClusterFailover(const RespValue& cmd);
    std::string handleClusterSetSlot(const RespValue& cmd);
    std::string handleClusterMigrate(const RespValue& cmd);
    std::string handleReplicationCommand(const std::string& cmd_name, const RespValue& cmd);
    void replicateWrite(const std::vector<std::string>& command) const;
    std::vector<std::string> clusterNodes() const;

    CacheStore& cache_;
    FixedMemoryPool& memory_pool_;
    const AppConfig& config_;
    bool cluster_mode_;
    std::string current_node_;
    ClusterSlotMap* slot_map_;
    std::mutex* slot_map_mutex_;
    AppendOnlyFile* aof_;
    std::function<void()> cluster_change_callback_;
    ReplicationBacklog* replication_backlog_;
    std::atomic<uint64_t>* replication_offset_;
    std::function<void(uint64_t)> replication_offset_callback_;
    ReplicationDispatcher* replication_dispatcher_;
    std::mutex* replication_apply_mutex_;
    std::function<void()> replica_promoted_callback_;
    const std::string* local_replid_;
    std::string* upstream_replid_;
    std::atomic<bool>* primary_state_;
    std::atomic<bool>* writes_allowed_;
    std::function<std::string()> active_master_callback_;
    std::function<std::string(const RespValue&)> failover_command_callback_;
    std::function<std::string()> failover_info_callback_;
    std::atomic<bool> promoted_to_master_{false};
};

} // namespace miniredis
