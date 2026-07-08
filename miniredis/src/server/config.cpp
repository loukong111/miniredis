#include "miniredis/server/config.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace miniredis {
namespace {

std::string toLower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

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

std::string trim(const std::string& value) {
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

std::string normalizeKey(std::string key) {
    key = trim(key);
    for (char& c : key) {
        if (c == '-') c = '_';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool parseBool(const std::string& value) {
    std::string lowered = toLower(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    throw std::invalid_argument("invalid boolean value: " + value);
}

AclUser parseAclUser(std::string value) {
    value = unquote(std::move(value));
    AclUser user;

    size_t first_colon = value.find(':');
    size_t second_colon = first_colon == std::string::npos
                              ? std::string::npos
                              : value.find(':', first_colon + 1);
    if (first_colon != std::string::npos && second_colon != std::string::npos) {
        user.username = trim(value.substr(0, first_colon));
        user.password = value.substr(first_colon + 1, second_colon - first_colon - 1);
        std::string role_text = trim(value.substr(second_colon + 1));
        if (!parseAclRole(role_text, user.role)) {
            throw std::invalid_argument("invalid ACL role: " + role_text);
        }
    } else {
        std::istringstream iss(value);
        if (!(iss >> user.username)) {
            throw std::invalid_argument("invalid ACL user");
        }
        std::string token;
        bool has_password = false;
        bool has_role = false;
        while (iss >> token) {
            size_t eq = token.find('=');
            if (eq == std::string::npos) {
                throw std::invalid_argument("invalid ACL token: " + token);
            }
            std::string key = normalizeKey(token.substr(0, eq));
            std::string val = unquote(token.substr(eq + 1));
            if (key == "password" || key == "pass") {
                user.password = val;
                has_password = true;
            } else if (key == "role") {
                if (!parseAclRole(val, user.role)) {
                    throw std::invalid_argument("invalid ACL role: " + val);
                }
                has_role = true;
            } else {
                throw std::invalid_argument("invalid ACL field: " + key);
            }
        }
        if (!has_password || !has_role) {
            throw std::invalid_argument("ACL user requires password and role");
        }
    }

    if (user.username.empty() || user.password.empty()) {
        throw std::invalid_argument("ACL username/password must not be empty");
    }
    return user;
}

void parseAclUsersList(AppConfig& config, const std::string& value) {
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(';', start);
        std::string item = trim(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!item.empty()) {
            config.acl_users.push_back(parseAclUser(item));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

std::string findConfigPath(int argc, char* argv[]) {
    std::string path = getEnvOr("MINIREDIS_CONFIG", "");
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--config" && i + 1 < argc) {
            path = argv[i + 1];
            ++i;
        } else if (arg.rfind("--config=", 0) == 0) {
            path = arg.substr(std::string("--config=").size());
        }
    }
    return path;
}

void applyConfigValue(AppConfig& config, const std::string& raw_key, const std::string& raw_value) {
    std::string key = normalizeKey(raw_key);
    std::string value = unquote(raw_value);

    if (key == "bind" || key == "bind_addr") config.bind_addr = value;
    else if (key == "log_file") config.log_file = value;
    else if (key == "log_level") config.log_level = value;
    else if (key == "port") config.port = std::stoi(value);
    else if (key == "stats_bind" || key == "stats_bind_addr") config.stats_bind_addr = value;
    else if (key == "stats_port") config.stats_port = std::stoi(value);
    else if (key == "snapshot_file") config.snapshot_file = value;
    else if (key == "snapshot_interval" || key == "snapshot_interval_sec") config.snapshot_interval_sec = std::stoi(value);
    else if (key == "appendonly_file" || key == "aof_file") config.appendonly_file = value;
    else if (key == "appendfsync") config.appendfsync = value;
    else if (key == "replicaof") config.replicaof = value;
    else if (key == "replicas" || key == "replicas_str") config.replicas_str = value;
    else if (key == "requirepass") config.requirepass = value;
    else if (key == "acl_user" || key == "user") config.acl_users.push_back(parseAclUser(value));
    else if (key == "acl_users") parseAclUsersList(config, value);
    else if (key == "max_request_bytes") config.max_request_bytes = static_cast<size_t>(std::stoull(value));
    else if (key == "max_key_bytes") config.max_key_bytes = static_cast<size_t>(std::stoull(value));
    else if (key == "max_value_bytes") config.max_value_bytes = static_cast<size_t>(std::stoull(value));
    else if (key == "max_pipeline_commands") config.max_pipeline_commands = static_cast<size_t>(std::stoull(value));
    else if (key == "client_output_buffer_limit") config.client_output_buffer_limit = static_cast<size_t>(std::stoull(value));
    else if (key == "max_clients") config.max_clients = static_cast<size_t>(std::stoull(value));
    else if (key == "io_threads") config.io_threads = static_cast<size_t>(std::stoull(value));
    else if (key == "cache_shards") config.cache_shards = static_cast<size_t>(std::stoull(value));
    else if (key == "maxmemory" || key == "maxmemory_bytes") config.maxmemory_bytes = static_cast<size_t>(std::stoull(value));
    else if (key == "eviction_policy") config.eviction_policy = value;
    else if (key == "slowlog_log_slower_than_us") config.slowlog_log_slower_than_us = static_cast<size_t>(std::stoull(value));
    else if (key == "slowlog_max_len") config.slowlog_max_len = static_cast<size_t>(std::stoull(value));
    else if (key == "cluster" || key == "cluster_mode") config.cluster_mode = parseBool(value);
    else if (key == "enable_node_discovery") config.enable_node_discovery = parseBool(value);
    else if (key == "cluster_heartbeat" || key == "cluster_heartbeat_interval" || key == "cluster_heartbeat_interval_sec") {
        config.cluster_heartbeat_interval_sec = std::stoi(value);
    } else if (key == "cluster_fail_threshold") config.cluster_fail_threshold = std::stoi(value);
    else if (key == "cluster_config_file") config.cluster_config_file = value;
    else if (key == "node_addr") config.node_addr = value;
    else if (key == "nodes" || key == "nodes_str") config.nodes_str = value;
    else if (key == "mysql_host") config.mysql_host = value;
    else if (key == "mysql_user") config.mysql_user = value;
    else if (key == "mysql_pass") config.mysql_pass = value;
    else if (key == "mysql_db") config.mysql_db = value;
    else if (key == "mysql_port") config.mysql_port = std::stoi(value);
    else throw std::invalid_argument("unknown config key: " + raw_key);
}

bool loadConfigFile(AppConfig& config, const std::string& path) {
    if (path.empty()) return true;

    std::ifstream input(path);
    if (!input) {
        std::cerr << "Failed to open config file: " << path << std::endl;
        return false;
    }

    std::string line;
    size_t line_no = 0;
    try {
        while (std::getline(input, line)) {
            ++line_no;
            size_t comment = line.find('#');
            if (comment != std::string::npos) {
                line = line.substr(0, comment);
            }
            line = trim(line);
            if (line.empty()) continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                throw std::invalid_argument("expected key=value");
            }
            applyConfigValue(config, line.substr(0, eq), line.substr(eq + 1));
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid config file " << path << ":" << line_no
                  << ": " << e.what() << std::endl;
        return false;
    }

    config.config_file = path;
    return true;
}

void loadEnvironment(AppConfig& config) {
    config.bind_addr = getEnvOr("MINIREDIS_BIND", config.bind_addr);
    config.log_file = getEnvOr("MINIREDIS_LOG_FILE", config.log_file);
    config.log_level = getEnvOr("MINIREDIS_LOG_LEVEL", config.log_level);
    config.port = getEnvIntOr("MINIREDIS_PORT", config.port);
    config.stats_bind_addr = getEnvOr("MINIREDIS_STATS_BIND", config.stats_bind_addr);
    config.stats_port = getEnvIntOr("MINIREDIS_STATS_PORT", config.stats_port);
    config.snapshot_file = getEnvOr("MINIREDIS_SNAPSHOT_FILE", config.snapshot_file);
    config.snapshot_interval_sec = getEnvIntOr("MINIREDIS_SNAPSHOT_INTERVAL", config.snapshot_interval_sec);
    config.appendonly_file = getEnvOr("MINIREDIS_APPENDONLY_FILE", config.appendonly_file);
    config.appendfsync = getEnvOr("MINIREDIS_APPENDFSYNC", config.appendfsync);
    config.replicaof = getEnvOr("MINIREDIS_REPLICAOF", config.replicaof);
    config.replicas_str = getEnvOr("MINIREDIS_REPLICAS", config.replicas_str);
    config.requirepass = getEnvOr("MINIREDIS_REQUIREPASS", config.requirepass);
    const char* acl_users = std::getenv("MINIREDIS_ACL_USERS");
    if (acl_users) {
        parseAclUsersList(config, acl_users);
    }
    config.max_request_bytes = getEnvSizeOr("MINIREDIS_MAX_REQUEST_BYTES", config.max_request_bytes);
    config.max_key_bytes = getEnvSizeOr("MINIREDIS_MAX_KEY_BYTES", config.max_key_bytes);
    config.max_value_bytes = getEnvSizeOr("MINIREDIS_MAX_VALUE_BYTES", config.max_value_bytes);
    config.max_pipeline_commands = getEnvSizeOr("MINIREDIS_MAX_PIPELINE_COMMANDS", config.max_pipeline_commands);
    config.client_output_buffer_limit = getEnvSizeOr("MINIREDIS_CLIENT_OUTPUT_BUFFER_LIMIT", config.client_output_buffer_limit);
    config.max_clients = getEnvSizeOr("MINIREDIS_MAX_CLIENTS", config.max_clients);
    config.io_threads = getEnvSizeOr("MINIREDIS_IO_THREADS", config.io_threads);
    config.cache_shards = getEnvSizeOr("MINIREDIS_CACHE_SHARDS", config.cache_shards);
    config.maxmemory_bytes = getEnvSizeOr("MINIREDIS_MAXMEMORY", config.maxmemory_bytes);
    config.eviction_policy = getEnvOr("MINIREDIS_EVICTION_POLICY", config.eviction_policy);
    config.slowlog_log_slower_than_us = getEnvSizeOr("MINIREDIS_SLOWLOG_LOG_SLOWER_THAN_US", config.slowlog_log_slower_than_us);
    config.slowlog_max_len = getEnvSizeOr("MINIREDIS_SLOWLOG_MAX_LEN", config.slowlog_max_len);
    config.cluster_heartbeat_interval_sec = getEnvIntOr("MINIREDIS_CLUSTER_HEARTBEAT_INTERVAL", config.cluster_heartbeat_interval_sec);
    config.cluster_fail_threshold = getEnvIntOr("MINIREDIS_CLUSTER_FAIL_THRESHOLD", config.cluster_fail_threshold);
    config.cluster_config_file = getEnvOr("MINIREDIS_CLUSTER_CONFIG_FILE", config.cluster_config_file);
    config.mysql_host = getEnvOr("MINIREDIS_MYSQL_HOST", config.mysql_host);
    config.mysql_user = getEnvOr("MINIREDIS_MYSQL_USER", config.mysql_user);
    config.mysql_pass = getEnvOr("MINIREDIS_MYSQL_PASS", config.mysql_pass);
    config.mysql_db = getEnvOr("MINIREDIS_MYSQL_DB", config.mysql_db);
    config.mysql_port = getEnvIntOr("MINIREDIS_MYSQL_PORT", config.mysql_port);
}

} // namespace

bool parseAclRole(const std::string& value, AclRole& role) {
    std::string normalized = toLower(trim(value));
    if (normalized == "admin") {
        role = AclRole::Admin;
        return true;
    }
    if (normalized == "readwrite" || normalized == "rw" || normalized == "writer") {
        role = AclRole::ReadWrite;
        return true;
    }
    if (normalized == "readonly" || normalized == "ro" || normalized == "reader") {
        role = AclRole::ReadOnly;
        return true;
    }
    return false;
}

std::string aclRoleName(AclRole role) {
    switch (role) {
        case AclRole::Admin: return "admin";
        case AclRole::ReadWrite: return "readwrite";
        case AclRole::ReadOnly: return "readonly";
    }
    return "readonly";
}

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
        << "  --config path             Load key=value config file\n"
        << "  --log-file path           Append structured logs to file (default: stderr)\n"
        << "  --log-level level         debug, info, warn, or error (default: info)\n"
        << "  --bind ip                 Bind address (default: 127.0.0.1)\n"
        << "  --port port               RESP port (default: 6366)\n"
        << "  --stats-bind ip           Stats bind address (default: 127.0.0.1)\n"
        << "  --stats-port port         Stats HTTP port (default: 8080)\n"
        << "  --snapshot-file path      Snapshot file path\n"
        << "  --snapshot-interval sec   Snapshot interval (default: 20)\n"
        << "  --appendonly-file path    Enable AOF and append write commands to this file\n"
        << "  --appendfsync policy      no, everysec, or always (default: everysec)\n"
        << "  --replicaof ip:port       Run as a read-only replica of this master\n"
        << "  --replicas a:port,b:port  Master-side replicas to forward write commands\n"
        << "  --requirepass password    Enable AUTH with this password\n"
        << "  --acl-user user:pass:role Add ACL user, role=admin|readwrite|readonly\n"
        << "  --max-request-bytes bytes Maximum buffered request bytes per client (default: 16777216)\n"
        << "  --max-key-bytes bytes     Maximum key length in bytes (default: 4096)\n"
        << "  --max-value-bytes bytes   Maximum value length in bytes (default: 16777216)\n"
        << "  --max-pipeline-commands count  Maximum commands processed per read batch (default: 1024)\n"
        << "  --client-output-buffer-limit bytes  Maximum pending response bytes per client (default: 33554432)\n"
        << "  --max-clients count       Maximum concurrent RESP clients (default: 10000)\n"
        << "  --io-threads count        Worker reactor threads for client IO (default: 4)\n"
        << "  --cache-shards count      Cache shards used to reduce KV lock contention (default: 16)\n"
        << "  --maxmemory bytes         Maximum cache payload bytes (0 means unlimited)\n"
        << "  --eviction-policy policy  noeviction or lru (default: noeviction)\n"
        << "  --slowlog-log-slower-than-us us  Slowlog threshold in microseconds (default: 10000, 0 disables)\n"
        << "  --slowlog-max-len count   Maximum slowlog entries kept in memory (default: 128)\n"
        << "  --cluster                 Enable experimental cluster routing\n"
        << "  --enable-node-discovery   Enable MySQL-backed cluster node discovery\n"
        << "  --cluster-heartbeat sec   Cluster heartbeat interval (default: 2)\n"
        << "  --cluster-fail-threshold count  Failed heartbeats before marking fail (default: 3)\n"
        << "  --cluster-config-file path  Persist and restore cluster slots/node states\n"
        << "  --node-addr ip:port       Current cluster node address\n"
        << "  --nodes a:port,b:port     Initial cluster nodes\n"
        << "  --mysql-host host         MySQL host for cluster discovery\n"
        << "  --mysql-user user         MySQL user for cluster discovery\n"
        << "  --mysql-pass password     MySQL password for cluster discovery\n"
        << "  --mysql-db db             MySQL database for cluster discovery\n"
        << "  --mysql-port port         MySQL port for cluster discovery\n";
}

ConfigParseResult parseConfig(int argc, char* argv[], AppConfig& config) {
    std::string config_path = findConfigPath(argc, argv);
    if (!loadConfigFile(config, config_path)) {
        return ConfigParseResult::Error;
    }
    try {
        loadEnvironment(config);
    } catch (const std::exception& e) {
        std::cerr << "Invalid environment configuration: " << e.what() << std::endl;
        return ConfigParseResult::Error;
    }

    static struct option long_options[] = {
        {"config",            required_argument, 0, 1015},
        {"log-file",          required_argument, 0, 1016},
        {"log-level",         required_argument, 0, 1017},
        {"cluster",           no_argument,       0, 'c'},
        {"enable-node-discovery", no_argument,   0, 1007},
        {"cluster-heartbeat", required_argument, 0, 1009},
        {"cluster-fail-threshold", required_argument, 0, 1010},
        {"cluster-config-file", required_argument, 0, 1022},
        {"node-addr",         required_argument, 0, 'a'},
        {"nodes",             required_argument, 0, 'n'},
        {"bind",              required_argument, 0, 'b'},
        {"port",              required_argument, 0, 'p'},
        {"stats-bind",        required_argument, 0, 1000},
        {"stats-port",        required_argument, 0, 1001},
        {"snapshot-file",     required_argument, 0, 's'},
        {"snapshot-interval", required_argument, 0, 'i'},
        {"appendonly-file",   required_argument, 0, 1020},
        {"appendfsync",       required_argument, 0, 1021},
        {"replicaof",         required_argument, 0, 1023},
        {"replicas",          required_argument, 0, 1024},
        {"requirepass",       required_argument, 0, 'r'},
        {"acl-user",          required_argument, 0, 1025},
        {"max-request-bytes",  required_argument, 0, 1026},
        {"max-key-bytes",      required_argument, 0, 1027},
        {"max-value-bytes",    required_argument, 0, 1028},
        {"max-pipeline-commands", required_argument, 0, 1029},
        {"client-output-buffer-limit", required_argument, 0, 1030},
        {"max-clients",       required_argument, 0, 1008},
        {"io-threads",        required_argument, 0, 1019},
        {"cache-shards",      required_argument, 0, 1018},
        {"maxmemory",         required_argument, 0, 1011},
        {"eviction-policy",   required_argument, 0, 1012},
        {"slowlog-log-slower-than-us", required_argument, 0, 1013},
        {"slowlog-max-len",   required_argument, 0, 1014},
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
                case 1009: config.cluster_heartbeat_interval_sec = std::stoi(optarg); break;
                case 1010: config.cluster_fail_threshold = std::stoi(optarg); break;
                case 1022: config.cluster_config_file = optarg; break;
                case 'a': config.node_addr = optarg; break;
                case 'n': config.nodes_str = optarg; break;
                case 'b': config.bind_addr = optarg; break;
                case 'p': config.port = std::stoi(optarg); break;
                case 's': config.snapshot_file = optarg; break;
                case 'i': config.snapshot_interval_sec = std::stoi(optarg); break;
                case 1020: config.appendonly_file = optarg; break;
                case 1021: config.appendfsync = optarg; break;
                case 1023: config.replicaof = optarg; break;
                case 1024: config.replicas_str = optarg; break;
                case 'r': config.requirepass = optarg; break;
                case 1025: config.acl_users.push_back(parseAclUser(optarg)); break;
                case 1026: config.max_request_bytes = static_cast<size_t>(std::stoull(optarg)); break;
                case 1027: config.max_key_bytes = static_cast<size_t>(std::stoull(optarg)); break;
                case 1028: config.max_value_bytes = static_cast<size_t>(std::stoull(optarg)); break;
                case 1029: config.max_pipeline_commands = static_cast<size_t>(std::stoull(optarg)); break;
                case 1030: config.client_output_buffer_limit = static_cast<size_t>(std::stoull(optarg)); break;
                case 1008: config.max_clients = static_cast<size_t>(std::stoull(optarg)); break;
                case 1019: config.io_threads = static_cast<size_t>(std::stoull(optarg)); break;
                case 1018: config.cache_shards = static_cast<size_t>(std::stoull(optarg)); break;
                case 1011: config.maxmemory_bytes = static_cast<size_t>(std::stoull(optarg)); break;
                case 1012: config.eviction_policy = optarg; break;
                case 1013: config.slowlog_log_slower_than_us = static_cast<size_t>(std::stoull(optarg)); break;
                case 1014: config.slowlog_max_len = static_cast<size_t>(std::stoull(optarg)); break;
                case 1015: config.config_file = optarg; break;
                case 1016: config.log_file = optarg; break;
                case 1017: config.log_level = optarg; break;
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
    if (config.max_request_bytes == 0 || config.max_key_bytes == 0 ||
        config.max_value_bytes == 0 || config.max_pipeline_commands == 0 ||
        config.client_output_buffer_limit == 0) {
        std::cerr << "resource limits must be positive" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.io_threads == 0 || config.io_threads > 128) {
        std::cerr << "io threads must be in range 1..128" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.cache_shards == 0 || config.cache_shards > 1024) {
        std::cerr << "cache shards must be in range 1..1024" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.eviction_policy != "noeviction" && config.eviction_policy != "lru") {
        std::cerr << "eviction policy must be noeviction or lru" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.log_level != "debug" && config.log_level != "info" &&
        config.log_level != "warn" && config.log_level != "warning" &&
        config.log_level != "error") {
        std::cerr << "log level must be debug, info, warn, or error" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.appendfsync != "no" && config.appendfsync != "everysec" &&
        config.appendfsync != "always") {
        std::cerr << "appendfsync must be no, everysec, or always" << std::endl;
        return ConfigParseResult::Error;
    }
    if (config.cluster_heartbeat_interval_sec <= 0 || config.cluster_fail_threshold <= 0) {
        std::cerr << "cluster heartbeat and fail threshold must be positive" << std::endl;
        return ConfigParseResult::Error;
    }
    for (size_t i = 0; i < config.acl_users.size(); ++i) {
        for (size_t j = i + 1; j < config.acl_users.size(); ++j) {
            if (config.acl_users[i].username == config.acl_users[j].username) {
                std::cerr << "duplicate ACL user: " << config.acl_users[i].username << std::endl;
                return ConfigParseResult::Error;
            }
        }
    }
    return ConfigParseResult::Ok;
}

} // namespace miniredis
