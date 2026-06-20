#include "miniredis/server/config.hpp"
#include <cstdlib>
#include <getopt.h>
#include <iostream>

namespace miniredis {
namespace {

std::string getEnvOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

int getEnvIntOr(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

size_t getEnvSizeOr(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return static_cast<size_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

void loadEnvironment(AppConfig& config) {
    config.bind_addr = getEnvOr("MINIREDIS_BIND", config.bind_addr);
    config.port = getEnvIntOr("MINIREDIS_PORT", config.port);
    config.stats_bind_addr = getEnvOr("MINIREDIS_STATS_BIND", config.stats_bind_addr);
    config.stats_port = getEnvIntOr("MINIREDIS_STATS_PORT", config.stats_port);
    config.snapshot_file = getEnvOr("MINIREDIS_SNAPSHOT_FILE", config.snapshot_file);
    config.snapshot_interval_sec = getEnvIntOr("MINIREDIS_SNAPSHOT_INTERVAL", config.snapshot_interval_sec);
    config.requirepass = getEnvOr("MINIREDIS_REQUIREPASS", config.requirepass);
    config.max_clients = getEnvSizeOr("MINIREDIS_MAX_CLIENTS", config.max_clients);
    config.mysql_host = getEnvOr("MINIREDIS_MYSQL_HOST", config.mysql_host);
    config.mysql_user = getEnvOr("MINIREDIS_MYSQL_USER", config.mysql_user);
    config.mysql_pass = getEnvOr("MINIREDIS_MYSQL_PASS", config.mysql_pass);
    config.mysql_db = getEnvOr("MINIREDIS_MYSQL_DB", config.mysql_db);
    config.mysql_port = getEnvIntOr("MINIREDIS_MYSQL_PORT", config.mysql_port);
}

} // namespace

bool parseNodePort(const std::string& addr, int& port) {
    size_t colon = addr.find(':');
    if (colon == std::string::npos) return false;
    try {
        port = std::stoi(addr.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return port > 0 && port <= 65535;
}

void printUsage(const char* program) {
    std::cerr
        << "Usage: " << program << " [options]\n"
        << "  --bind ip                 Bind address (default: 127.0.0.1)\n"
        << "  --port port               RESP port (default: 6366)\n"
        << "  --stats-bind ip           Stats bind address (default: 127.0.0.1)\n"
        << "  --stats-port port         Stats HTTP port (default: 8080)\n"
        << "  --snapshot-file path      Snapshot file path\n"
        << "  --snapshot-interval sec   Snapshot interval (default: 20)\n"
        << "  --requirepass password    Enable AUTH with this password\n"
        << "  --max-clients count       Maximum concurrent RESP clients (default: 10000)\n"
        << "  --cluster                 Enable experimental cluster routing\n"
        << "  --enable-node-discovery   Enable MySQL-backed cluster node discovery\n"
        << "  --node-addr ip:port       Current cluster node address\n"
        << "  --nodes a:port,b:port     Initial cluster nodes\n"
        << "  --mysql-host host         MySQL host for cluster discovery\n"
        << "  --mysql-user user         MySQL user for cluster discovery\n"
        << "  --mysql-pass password     MySQL password for cluster discovery\n"
        << "  --mysql-db db             MySQL database for cluster discovery\n"
        << "  --mysql-port port         MySQL port for cluster discovery\n";
}

ConfigParseResult parseConfig(int argc, char* argv[], AppConfig& config) {
    loadEnvironment(config);

    static struct option long_options[] = {
        {"cluster",           no_argument,       0, 'c'},
        {"enable-node-discovery", no_argument,   0, 1007},
        {"node-addr",         required_argument, 0, 'a'},
        {"nodes",             required_argument, 0, 'n'},
        {"bind",              required_argument, 0, 'b'},
        {"port",              required_argument, 0, 'p'},
        {"stats-bind",        required_argument, 0, 1000},
        {"stats-port",        required_argument, 0, 1001},
        {"snapshot-file",     required_argument, 0, 's'},
        {"snapshot-interval", required_argument, 0, 'i'},
        {"requirepass",       required_argument, 0, 'r'},
        {"max-clients",       required_argument, 0, 1008},
        {"mysql-host",        required_argument, 0, 1002},
        {"mysql-user",        required_argument, 0, 1003},
        {"mysql-pass",        required_argument, 0, 1004},
        {"mysql-db",          required_argument, 0, 1005},
        {"mysql-port",        required_argument, 0, 1006},
        {"help",              no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 1;
    int opt;
    try {
        while ((opt = getopt_long(argc, argv, "ca:n:b:p:s:i:r:h", long_options, nullptr)) != -1) {
            switch (opt) {
                case 'c': config.cluster_mode = true; break;
                case 1007: config.enable_node_discovery = true; break;
                case 'a': config.node_addr = optarg; break;
                case 'n': config.nodes_str = optarg; break;
                case 'b': config.bind_addr = optarg; break;
                case 'p': config.port = std::stoi(optarg); break;
                case 's': config.snapshot_file = optarg; break;
                case 'i': config.snapshot_interval_sec = std::stoi(optarg); break;
                case 'r': config.requirepass = optarg; break;
                case 1008: config.max_clients = static_cast<size_t>(std::stoull(optarg)); break;
                case 1000: config.stats_bind_addr = optarg; break;
                case 1001: config.stats_port = std::stoi(optarg); break;
                case 1002: config.mysql_host = optarg; break;
                case 1003: config.mysql_user = optarg; break;
                case 1004: config.mysql_pass = optarg; break;
                case 1005: config.mysql_db = optarg; break;
                case 1006: config.mysql_port = std::stoi(optarg); break;
                case 'h':
                    printUsage(argv[0]);
                    return ConfigParseResult::Help;
                default:
                    printUsage(argv[0]);
                    return ConfigParseResult::Error;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid configuration value: " << e.what() << std::endl;
        return ConfigParseResult::Error;
    }

    if (config.port <= 0 || config.port > 65535 ||
        config.stats_port <= 0 || config.stats_port > 65535 ||
        config.mysql_port <= 0 || config.mysql_port > 65535) {
        std::cerr << "Invalid port configuration" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.snapshot_interval_sec <= 0) {
        std::cerr << "snapshot interval must be positive" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.max_clients == 0) {
        std::cerr << "max clients must be positive" << std::endl;
        return ConfigParseResult::Error;
    }
    return ConfigParseResult::Ok;
}

} // namespace miniredis
