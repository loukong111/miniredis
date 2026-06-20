#pragma once

#include <cstddef>
#include <string>

namespace miniredis {

struct AppConfig {
    std::string bind_addr = "127.0.0.1";
    int port = 6366;
    std::string stats_bind_addr = "127.0.0.1";
    int stats_port = 8080;
    std::string snapshot_file;
    int snapshot_interval_sec = 20;
    std::string requirepass;
    size_t max_request_bytes = 16 * 1024 * 1024;
    size_t max_clients = 10000;

    bool cluster_mode = false;
    bool enable_node_discovery = false;
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

} // namespace miniredis
