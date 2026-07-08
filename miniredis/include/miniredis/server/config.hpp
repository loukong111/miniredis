#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace miniredis {

enum class AclRole {
    Admin,
    ReadWrite,
    ReadOnly
};

struct AclUser {
    std::string username;
    std::string password;
    AclRole role = AclRole::ReadOnly;
};

struct AppConfig {
    std::string config_file;
    std::string log_file;
    std::string log_level = "info";
    std::string bind_addr = "127.0.0.1";
    int port = 6366;
    std::string stats_bind_addr = "127.0.0.1";
    int stats_port = 8080;
    std::string snapshot_file;
    int snapshot_interval_sec = 20;
    std::string appendonly_file;
    std::string appendfsync = "everysec";
    std::string replicaof;
    std::string replicas_str;
    std::string requirepass;
    std::vector<AclUser> acl_users;
    size_t max_request_bytes = 16 * 1024 * 1024;
    size_t max_key_bytes = 4 * 1024;
    size_t max_value_bytes = 16 * 1024 * 1024;
    size_t max_pipeline_commands = 1024;
    size_t client_output_buffer_limit = 32 * 1024 * 1024;
    size_t max_clients = 10000;
    size_t io_threads = 4;
    size_t cache_shards = 16;
    size_t maxmemory_bytes = 0;
    std::string eviction_policy = "noeviction";
    size_t slowlog_log_slower_than_us = 10000;
    size_t slowlog_max_len = 128;

    bool cluster_mode = false;
    bool enable_node_discovery = false;
    int cluster_heartbeat_interval_sec = 2;
    int cluster_fail_threshold = 3;
    std::string cluster_config_file;
    std::string node_addr;
    std::string nodes_str;

    std::string mysql_host = "127.0.0.1";
    std::string mysql_user = "miniredis";
    std::string mysql_pass;
    std::string mysql_db = "miniredis";
    int mysql_port = 3306;
};

enum class ConfigParseResult {
    Ok,
    Help,
    Error
};

ConfigParseResult parseConfig(int argc, char* argv[], AppConfig& config);
void printUsage(const char* program);
bool parseNodePort(const std::string& addr, int& port);
bool parseAclRole(const std::string& value, AclRole& role);
std::string aclRoleName(AclRole role);

} // namespace miniredis
