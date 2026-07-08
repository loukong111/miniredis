#include "miniredis/server/command_handler.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/metrics/stats.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <utility>
#include <vector>
#include <unistd.h>

namespace miniredis {
namespace {

struct CommandSpec {
    const char* name;
    int arity;
    std::vector<const char*> flags;
    int first_key;
    int last_key;
    int step;
};

const std::vector<CommandSpec>& commandTable() {
    static const std::vector<CommandSpec> specs = {
        {"ping",  -1, {"fast"},                  0,  0, 0},
        {"auth",   2, {"noscript", "fast"},      0,  0, 0},
        {"acl",   -2, {"admin", "stale"},        0,  0, 0},
        {"asking", 1, {"fast"},                  0,  0, 0},
        {"set",   -3, {"write", "denyoom"},      1,  1, 1},
        {"setnx",  3, {"write", "denyoom", "fast"}, 1, 1, 1},
        {"get",    2, {"readonly", "fast"},      1,  1, 1},
        {"strlen", 2, {"readonly", "fast"},      1,  1, 1},
        {"append", 3, {"write", "denyoom"},      1,  1, 1},
        {"incr",   2, {"write", "denyoom", "fast"}, 1, 1, 1},
        {"decr",   2, {"write", "denyoom", "fast"}, 1, 1, 1},
        {"incrby", 3, {"write", "denyoom", "fast"}, 1, 1, 1},
        {"decrby", 3, {"write", "denyoom", "fast"}, 1, 1, 1},
        {"del",   -2, {"write"},                 1, -1, 1},
        {"exists",-2, {"readonly", "fast"},      1, -1, 1},
        {"mget",  -2, {"readonly", "fast"},      1, -1, 1},
        {"expire", 3, {"write", "fast"},         1,  1, 1},
        {"ttl",    2, {"readonly", "fast"},      1,  1, 1},
        {"command",-1, {"loading", "stale"},     0,  0, 0},
        {"info",  -1, {"loading", "stale"},      0,  0, 0},
        {"slowlog",-2, {"admin", "stale"},       0,  0, 0},
        {"bgrewriteaof", 1, {"admin", "write"},  0,  0, 0},
        {"replsnapshot", 1, {"admin", "readonly"}, 0, 0, 0},
        {"replfullsync", 1, {"admin", "readonly"}, 0, 0, 0},
        {"replpsync", 2, {"admin", "readonly"}, 0, 0, 0},
        {"replapply", -3, {"admin", "write"}, 0, 0, 0},
        {"cluster",-2, {"admin", "stale"},       0,  0, 0},
    };
    return specs;
}

std::string respArray(const std::vector<std::string>& encoded_items) {
    std::string result = "*" + std::to_string(encoded_items.size()) + "\r\n";
    for (const auto& item : encoded_items) {
        result += item;
    }
    return result;
}

std::string commandSpecResponse(const CommandSpec& spec) {
    std::vector<std::string> flags;
    flags.reserve(spec.flags.size());
    for (const char* flag : spec.flags) {
        flags.push_back(RespWriter::bulkString(flag));
    }

    return respArray({
        RespWriter::bulkString(spec.name),
        RespWriter::integer(spec.arity),
        respArray(flags),
        RespWriter::integer(spec.first_key),
        RespWriter::integer(spec.last_key),
        RespWriter::integer(spec.step),
    });
}

std::string commandTableResponse() {
    std::vector<std::string> commands;
    commands.reserve(commandTable().size());
    for (const auto& spec : commandTable()) {
        commands.push_back(commandSpecResponse(spec));
    }
    return respArray(commands);
}

std::string replicationSnapshotResponse(const SnapshotData& data) {
    std::vector<std::string> entries;
    entries.reserve(data.size());
    for (const auto& [key, entry] : data) {
        entries.push_back(respArray({
            RespWriter::bulkString(key),
            RespWriter::bulkString(entry.value),
            RespWriter::integer(static_cast<long long>(entry.expire_at_ms)),
        }));
    }
    return respArray(entries);
}

std::string replicationFullSyncResponse(uint64_t offset, const SnapshotData& data) {
    return respArray({
        RespWriter::integer(static_cast<long long>(offset)),
        replicationSnapshotResponse(data),
    });
}

std::string replicationBacklogEntryResponse(const ReplicationBacklogEntry& entry) {
    std::vector<std::string> command_parts;
    command_parts.reserve(entry.command.size());
    for (const auto& part : entry.command) {
        command_parts.push_back(RespWriter::bulkString(part));
    }
    return respArray({
        RespWriter::integer(static_cast<long long>(entry.offset)),
        respArray(command_parts),
    });
}

std::string replicationPsyncResponse(uint64_t current_offset,
                                     const std::vector<ReplicationBacklogEntry>& entries) {
    std::vector<std::string> encoded_entries;
    encoded_entries.reserve(entries.size());
    for (const auto& entry : entries) {
        encoded_entries.push_back(replicationBacklogEntryResponse(entry));
    }
    return respArray({
        RespWriter::bulkString("CONTINUE"),
        RespWriter::integer(static_cast<long long>(current_offset)),
        respArray(encoded_entries),
    });
}

std::string replicationFullResyncResponse(uint64_t current_offset) {
    return respArray({
        RespWriter::bulkString("FULLRESYNC"),
        RespWriter::integer(static_cast<long long>(current_offset)),
    });
}

std::string clusterSlotMapResponse(const ClusterSlotMapSnapshot& snapshot) {
    std::vector<std::string> node_items;
    node_items.reserve(snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        auto it = snapshot.node_states.find(node);
        ClusterNodeState state = (it == snapshot.node_states.end())
                                     ? ClusterNodeState::Healthy
                                     : it->second;
        node_items.push_back(respArray({
            RespWriter::bulkString(node),
            RespWriter::bulkString(clusterNodeStateName(state)),
        }));
    }

    std::vector<std::string> slot_ranges;
    bool in_range = false;
    uint16_t start = 0;
    std::string owner;
    for (uint16_t slot = 0; slot < kRedisClusterSlots; ++slot) {
        const std::string& current = snapshot.slot_owner[slot];
        if (!in_range && !current.empty()) {
            in_range = true;
            start = slot;
            owner = current;
        } else if (in_range && current != owner) {
            slot_ranges.push_back(respArray({
                RespWriter::integer(start),
                RespWriter::integer(static_cast<long long>(slot - 1)),
                RespWriter::bulkString(owner),
            }));
            in_range = false;
            if (!current.empty()) {
                in_range = true;
                start = slot;
                owner = current;
            }
        }
    }
    if (in_range) {
        slot_ranges.push_back(respArray({
            RespWriter::integer(start),
            RespWriter::integer(kRedisClusterSlots - 1),
            RespWriter::bulkString(owner),
        }));
    }

    std::vector<std::string> slot_states;
    for (size_t slot = 0; slot < snapshot.slot_meta.size(); ++slot) {
        const auto& meta = snapshot.slot_meta[slot];
        if (meta.state == ClusterSlotState::Stable) continue;
        slot_states.push_back(respArray({
            RespWriter::integer(static_cast<long long>(slot)),
            RespWriter::bulkString(clusterSlotStateName(meta.state)),
            RespWriter::bulkString(meta.peer_node),
        }));
    }

    return respArray({
        respArray({RespWriter::bulkString("epoch"),
                   RespWriter::integer(static_cast<long long>(snapshot.epoch))}),
        respArray({RespWriter::bulkString("nodes"), respArray(node_items)}),
        respArray({RespWriter::bulkString("slots"), respArray(slot_ranges)}),
        respArray({RespWriter::bulkString("slotstates"), respArray(slot_states)}),
    });
}

std::string toUpper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

bool parsePositiveInt(const std::string& value, int& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size() && out > 0;
}

bool parseInt64Strict(const std::string& value, long long& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size();
}

bool parseSlot(const std::string& value, int& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size() &&
           out >= 0 && out < kRedisClusterSlots;
}

bool parseNonNegativeSize(const std::string& value, size_t& out) {
    uint64_t parsed = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc() || ptr != value.data() + value.size()) return false;
    out = static_cast<size_t>(parsed);
    return static_cast<uint64_t>(out) == parsed;
}

bool parseU64Strict(const std::string& value, uint64_t& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size();
}

bool splitNodeAddr(const std::string& node, std::string& host, int& port);

std::string encodeRespCommand(const std::vector<std::string>& command) {
    std::string out = "*" + std::to_string(command.size()) + "\r\n";
    for (const auto& part : command) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out += part;
        out += "\r\n";
    }
    return out;
}

bool writeAllBlocking(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool readRespLine(int fd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n == 1) {
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                return true;
            }
            if (line.size() > 4096) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

bool sendClusterCommandToNode(const std::string& node,
                              const std::string& password,
                              const std::vector<std::string>& command,
                              std::string& response) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(node, host, port)) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    timeval timeout{};
    timeout.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    bool ok = true;
    if (!password.empty()) {
        ok = writeAllBlocking(fd, encodeRespCommand({"AUTH", password})) &&
             readRespLine(fd, response) && response.rfind("+OK", 0) == 0;
    }
    if (ok) {
        ok = writeAllBlocking(fd, encodeRespCommand(command)) && readRespLine(fd, response);
    }
    ::close(fd);
    return ok;
}

bool sendAskingCommandToNode(const std::string& node,
                             const std::string& password,
                             const std::vector<std::string>& command,
                             std::string& response) {
    std::string host;
    int port = 0;
    if (!splitNodeAddr(node, host, port)) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    timeval timeout{};
    timeout.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    bool ok = true;
    if (!password.empty()) {
        ok = writeAllBlocking(fd, encodeRespCommand({"AUTH", password})) &&
             readRespLine(fd, response) && response.rfind("+OK", 0) == 0;
    }
    if (ok) {
        ok = writeAllBlocking(fd, encodeRespCommand({"ASKING"})) &&
             readRespLine(fd, response) && response.rfind("+OK", 0) == 0;
    }
    if (ok) {
        ok = writeAllBlocking(fd, encodeRespCommand(command)) && readRespLine(fd, response);
    }
    ::close(fd);
    return ok;
}

std::vector<std::string> splitNodeList(const std::string& nodes_str) {
    std::vector<std::string> nodes;
    size_t start = 0;
    while (start <= nodes_str.size()) {
        size_t end = nodes_str.find(',', start);
        std::string node = nodes_str.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!node.empty()) nodes.push_back(std::move(node));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return nodes;
}

bool isClientWriteCommand(const std::string& cmd_name) {
    return cmd_name == "SET" || cmd_name == "DEL" || cmd_name == "EXPIRE" ||
           cmd_name == "SETNX" || cmd_name == "APPEND" || cmd_name == "INCR" ||
           cmd_name == "DECR" || cmd_name == "INCRBY" || cmd_name == "DECRBY" ||
           cmd_name == "BGREWRITEAOF";
}

bool constantTimeEquals(const std::string& lhs, const std::string& rhs) {
    const size_t max_len = std::max(lhs.size(), rhs.size());
    unsigned char diff = static_cast<unsigned char>(lhs.size() ^ rhs.size());
    for (size_t i = 0; i < max_len; ++i) {
        unsigned char a = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0;
        unsigned char b = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0;
        diff |= static_cast<unsigned char>(a ^ b);
    }
    return diff == 0;
}

bool splitNodeAddr(const std::string& node, std::string& host, int& port) {
    size_t pos = node.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= node.size()) return false;
    host = node.substr(0, pos);
    auto port_text = node.substr(pos + 1);
    auto [ptr, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    return ec == std::errc() && ptr == port_text.data() + port_text.size() &&
           port > 0 && port <= 65535;
}

std::string slowLogEntryResponse(const SlowLogEntry& entry) {
    std::vector<std::string> command;
    command.reserve(entry.command.size());
    for (const auto& part : entry.command) {
        command.push_back(RespWriter::bulkString(part));
    }
    return respArray({
        RespWriter::integer(static_cast<long long>(entry.id)),
        RespWriter::integer(static_cast<long long>(entry.unix_time)),
        RespWriter::integer(static_cast<long long>(entry.duration_us)),
        respArray(command),
    });
}

} // namespace

CommandHandler::CommandHandler(CacheStore& cache, FixedMemoryPool& memory_pool,
                               const AppConfig& config, bool cluster_mode,
                               std::string current_node,
                               ClusterSlotMap* slot_map,
                               std::mutex* slot_map_mutex,
                               AppendOnlyFile* aof,
                               std::function<void()> cluster_change_callback,
                               ReplicationBacklog* replication_backlog,
                               std::atomic<uint64_t>* replication_offset,
                               std::function<void(uint64_t)> replication_offset_callback)
    : cache_(cache),
      memory_pool_(memory_pool),
      config_(config),
      cluster_mode_(cluster_mode),
      current_node_(std::move(current_node)),
      slot_map_(slot_map),
      slot_map_mutex_(slot_map_mutex),
      aof_(aof),
      cluster_change_callback_(std::move(cluster_change_callback)),
      replication_backlog_(replication_backlog),
      replication_offset_(replication_offset),
      replication_offset_callback_(std::move(replication_offset_callback)) {}

bool CommandHandler::isReplica() const {
    return !config_.replicaof.empty() && !promoted_to_master_;
}

bool CommandHandler::authRequired() const {
    return !config_.requirepass.empty() || !config_.acl_users.empty();
}

bool CommandHandler::authenticate(const RespValue& cmd, CommandSession& session) const {
    if (cmd.array.size() == 2 && cmd.array[1].type == RespType::BULK_STRING) {
        if (!config_.requirepass.empty() &&
            constantTimeEquals(cmd.array[1].str, config_.requirepass)) {
            session.authenticated = true;
            session.role = AclRole::Admin;
            session.username = "default";
            return true;
        }
        return false;
    }

    if (cmd.array.size() == 3 &&
        cmd.array[1].type == RespType::BULK_STRING &&
        cmd.array[2].type == RespType::BULK_STRING) {
        const std::string& username = cmd.array[1].str;
        const std::string& password = cmd.array[2].str;
        for (const auto& user : config_.acl_users) {
            if (constantTimeEquals(user.username, username) &&
                constantTimeEquals(user.password, password)) {
                session.authenticated = true;
                session.role = user.role;
                session.username = user.username;
                return true;
            }
        }
        return false;
    }

    return false;
}

bool CommandHandler::isAllowed(const std::string& cmd_name, AclRole role) const {
    if (role == AclRole::Admin) return true;

    const bool readonly =
        cmd_name == "PING" || cmd_name == "GET" || cmd_name == "MGET" ||
        cmd_name == "STRLEN" || cmd_name == "TTL" || cmd_name == "EXISTS" || cmd_name == "INFO" ||
        cmd_name == "COMMAND" || cmd_name == "ACL" || cmd_name == "ASKING";
    if (role == AclRole::ReadOnly) return readonly;

    const bool write =
        cmd_name == "SET" || cmd_name == "SETNX" || cmd_name == "APPEND" ||
        cmd_name == "INCR" || cmd_name == "DECR" || cmd_name == "INCRBY" ||
        cmd_name == "DECRBY" || cmd_name == "DEL" || cmd_name == "EXPIRE";
    if (role == AclRole::ReadWrite) return readonly || write;

    return false;
}

std::string CommandHandler::validateKey(const RespValue& key) const {
    if (key.type != RespType::BULK_STRING) {
        return RespWriter::error("key must be bulk string");
    }
    if (key.str.size() > config_.max_key_bytes) {
        return RespWriter::error("key too large");
    }
    return {};
}

std::string CommandHandler::validateValue(const RespValue& value) const {
    if (value.type != RespType::BULK_STRING) {
        return RespWriter::error("value must be bulk string");
    }
    if (value.str.size() > config_.max_value_bytes) {
        return RespWriter::error("value too large");
    }
    return {};
}

std::string CommandHandler::handleAclCommand(const RespValue& cmd, const CommandSession& session) const {
    if (cmd.array.size() < 2 || cmd.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'acl'");
    }

    std::string subcmd = cmd.array[1].str;
    subcmd = toUpper(std::move(subcmd));

    if (subcmd == "WHOAMI") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'acl whoami'");
        }
        return RespWriter::bulkString(session.username.empty() ? "default" : session.username);
    }

    if (subcmd == "LIST") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'acl list'");
        }
        if (session.role != AclRole::Admin) {
            return RespWriter::error("NOPERM this user has no permissions to run the command");
        }

        std::vector<std::string> users;
        if (!config_.requirepass.empty()) {
            users.push_back(RespWriter::bulkString("user default role=admin password=*****"));
        }
        users.reserve(users.size() + config_.acl_users.size());
        for (const auto& user : config_.acl_users) {
            users.push_back(RespWriter::bulkString("user " + user.username +
                                                   " role=" + aclRoleName(user.role) +
                                                   " password=*****"));
        }
        return respArray(users);
    }

    if (subcmd == "HELP") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'acl help'");
        }
        return respArray({
            RespWriter::bulkString("ACL WHOAMI"),
            RespWriter::bulkString("ACL LIST"),
        });
    }

    return RespWriter::error("unsupported ACL subcommand '" + subcmd + "'");
}

void CommandHandler::refreshRuntimeStats() const {
    Stats::instance().setKeyCount(cache_.key_count());
    Stats::instance().setCacheShards(cache_.shard_count());
    Stats::instance().setIoThreads(config_.io_threads);
    Stats::instance().setMemoryPoolUsed(memory_pool_.used_blocks(), memory_pool_.free_blocks());
    Stats::instance().setCacheMemory(cache_.used_memory_bytes(),
                                     cache_.max_memory_bytes(),
                                     cache_.evicted_keys());
    Stats::instance().setResourceLimits(config_.max_request_bytes,
                                        config_.max_key_bytes,
                                        config_.max_value_bytes,
                                        config_.max_pipeline_commands,
                                        config_.client_output_buffer_limit,
                                        config_.max_clients);
}

std::string CommandHandler::routeIfNeeded(const std::string& cmd_name, const RespValue& cmd,
                                          CommandSession& session) const {
    bool needs_route = (cmd_name == "GET" || cmd_name == "SET" ||
                        cmd_name == "DEL" || cmd_name == "EXISTS" ||
                        cmd_name == "MGET" || cmd_name == "SETNX" ||
                        cmd_name == "APPEND" || cmd_name == "STRLEN" ||
                        cmd_name == "INCR" || cmd_name == "DECR" ||
                        cmd_name == "INCRBY" || cmd_name == "DECRBY" ||
                        cmd_name == "EXPIRE" ||
                        cmd_name == "TTL");
    if (!needs_route || !cluster_mode_) return {};
    if (cmd.array.size() < 2) {
        return RespWriter::error("wrong number of arguments for '" + cmd_name + "'");
    }
    if (!slot_map_ || !slot_map_mutex_) {
        return RespWriter::error("cluster slot map not initialized");
    }

    size_t last_key = 1;
    if (cmd_name == "DEL" || cmd_name == "EXISTS" || cmd_name == "MGET") {
        last_key = cmd.array.size() - 1;
    }

    uint16_t slot = 0;
    bool saw_key = false;
    for (size_t i = 1; i <= last_key; ++i) {
        const RespValue& key_arg = cmd.array[i];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        uint16_t key_slot = clusterHashSlot(key_arg.str);
        if (!saw_key) {
            slot = key_slot;
            saw_key = true;
        } else if (key_slot != slot) {
            return RespWriter::error("CROSSSLOT Keys in request don't hash to the same slot");
        }
    }

    std::string target;
    ClusterSlotMeta slot_meta;
    {
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        target = slot_map_->GetNodeForSlot(slot);
        slot_meta = slot_map_->GetSlotMeta(slot);
    }
    if (target.empty()) {
        return RespWriter::error("cluster has no active nodes");
    }
    if (target != current_node_) {
        if (slot_meta.state == ClusterSlotState::Importing &&
            slot_meta.peer_node == target) {
            if (session.asking) {
                session.asking = false;
                return {};
            }
            return "-MOVED " + std::to_string(slot) + " " + target + "\r\n";
        }
        return "-MOVED " + std::to_string(slot) + " " + target + "\r\n";
    }
    if (slot_meta.state == ClusterSlotState::Migrating && !slot_meta.peer_node.empty()) {
        return "-ASK " + std::to_string(slot) + " " + slot_meta.peer_node + "\r\n";
    }
    return {};
}

std::string CommandHandler::handleInfoCommand(const RespValue& cmd) const {
    if (cmd.array.size() > 2) {
        return RespWriter::error("wrong number of arguments for 'info'");
    }

    std::string section = "ALL";
    if (cmd.array.size() == 2) {
        if (cmd.array[1].type != RespType::BULK_STRING) {
            return RespWriter::error("section must be bulk string");
        }
        section = toUpper(cmd.array[1].str);
    }

    const bool all = (section == "ALL" || section == "DEFAULT");
    const bool wants_server = all || section == "SERVER";
    const bool wants_clients = all || section == "CLIENTS";
    const bool wants_memory = all || section == "MEMORY";
    const bool wants_stats = all || section == "STATS";
    const bool wants_persistence = all || section == "PERSISTENCE";
    const bool wants_replication = all || section == "REPLICATION";
    const bool wants_cluster = all || section == "CLUSTER";
    if (!wants_server && !wants_clients && !wants_memory && !wants_stats &&
        !wants_persistence && !wants_replication && !wants_cluster) {
        return RespWriter::error("unsupported INFO section '" + section + "'");
    }

    refreshRuntimeStats();
    StatsSnapshot stats = Stats::instance().snapshot();
    std::ostringstream oss;

    if (wants_server) {
        oss << "# Server\r\n";
        oss << "miniredis_version:0.1.0\r\n";
        oss << "redis_mode:" << (cluster_mode_ ? "cluster" : "standalone") << "\r\n";
        oss << "tcp_bind:" << config_.bind_addr << "\r\n";
        oss << "tcp_port:" << config_.port << "\r\n";
        oss << "io_threads:" << stats.io_threads << "\r\n";
        oss << "stats_bind:" << config_.stats_bind_addr << "\r\n";
        oss << "stats_port:" << config_.stats_port << "\r\n";
        oss << "uptime_in_seconds:" << stats.uptime_seconds << "\r\n";
        oss << "process_id:" << static_cast<long long>(::getpid()) << "\r\n";
        oss << "max_request_bytes:" << stats.max_request_bytes << "\r\n";
        oss << "max_pipeline_commands:" << stats.max_pipeline_commands << "\r\n";
        oss << "client_output_buffer_limit:" << stats.client_output_buffer_limit << "\r\n";
        oss << "\r\n";
    }

    if (wants_clients) {
        oss << "# Clients\r\n";
        oss << "connected_clients:" << stats.connected_clients << "\r\n";
        oss << "maxclients:" << config_.max_clients << "\r\n";
        oss << "total_connections_received:" << stats.total_connections << "\r\n";
        oss << "rejected_connections:" << stats.rejected_connections << "\r\n";
        oss << "\r\n";
    }

    if (wants_memory) {
        oss << "# Memory\r\n";
        oss << "used_memory:" << stats.used_memory_bytes << "\r\n";
        oss << "maxmemory:" << stats.maxmemory_bytes << "\r\n";
        oss << "maxmemory_policy:" << config_.eviction_policy << "\r\n";
        oss << "max_key_bytes:" << stats.max_key_bytes << "\r\n";
        oss << "max_value_bytes:" << stats.max_value_bytes << "\r\n";
        oss << "cache_shards:" << stats.cache_shards << "\r\n";
        oss << "mem_pool_used_blocks:" << stats.mem_pool_used_blocks << "\r\n";
        oss << "mem_pool_free_blocks:" << stats.mem_pool_free_blocks << "\r\n";
        oss << "evicted_keys:" << stats.evicted_keys << "\r\n";
        oss << "\r\n";
    }

    if (wants_stats) {
        oss << "# Stats\r\n";
        oss << "total_commands_processed:" << stats.total_commands << "\r\n";
        oss << "keyspace_hits:" << stats.get_hits << "\r\n";
        oss << "keyspace_misses:" << stats.get_misses << "\r\n";
        oss << "hit_rate:" << std::fixed << std::setprecision(4) << stats.hit_rate << "\r\n";
        oss << "key_count:" << stats.key_count << "\r\n";
        oss << "latency_samples:" << stats.latency_samples << "\r\n";
        oss << "avg_command_latency_us:" << stats.avg_command_latency_us << "\r\n";
        oss << "max_command_latency_us:" << stats.max_command_latency_us << "\r\n";
        oss << "slowlog_len:" << stats.slowlog_len << "\r\n";
        oss << "slowlog_log_slower_than_us:" << stats.slowlog_log_slower_than_us << "\r\n";
        oss << "slowlog_max_len:" << stats.slowlog_max_len << "\r\n";
        oss << "\r\n";
    }

    if (wants_persistence) {
        oss << "# Persistence\r\n";
        oss << "loading:0\r\n";
        oss << "snapshot_file:" << config_.snapshot_file << "\r\n";
        oss << "snapshot_interval_seconds:" << config_.snapshot_interval_sec << "\r\n";
        oss << "snapshot_format:MINIREDIS_SNAPSHOT_V2\r\n";
        oss << "ttl_aware_snapshot:1\r\n";
        oss << "snapshot_running:" << (stats.snapshot_running ? "1" : "0") << "\r\n";
        oss << "snapshot_last_success_unix_ms:" << stats.snapshot_last_success_unix_ms << "\r\n";
        oss << "snapshot_last_failure_unix_ms:" << stats.snapshot_last_failure_unix_ms << "\r\n";
        oss << "snapshot_last_duration_ms:" << stats.snapshot_last_duration_ms << "\r\n";
        oss << "snapshot_last_key_count:" << stats.snapshot_last_key_count << "\r\n";
        oss << "snapshot_failures:" << stats.snapshot_failures << "\r\n";
        oss << "appendonly_enabled:" << (config_.appendonly_file.empty() ? "0" : "1") << "\r\n";
        oss << "appendonly_file:" << config_.appendonly_file << "\r\n";
        oss << "appendfsync:" << config_.appendfsync << "\r\n";
        oss << "aof_rewrite_running:" << (stats.aof_rewrite_running ? "1" : "0") << "\r\n";
        oss << "aof_rewrite_buffer_bytes:" << stats.aof_rewrite_buffer_bytes << "\r\n";
        oss << "aof_last_rewrite_unix_ms:" << stats.aof_last_rewrite_unix_ms << "\r\n";
        oss << "aof_last_rewrite_failure_unix_ms:" << stats.aof_last_rewrite_failure_unix_ms << "\r\n";
        oss << "aof_last_rewrite_duration_ms:" << stats.aof_last_rewrite_duration_ms << "\r\n";
        oss << "aof_last_rewrite_records:" << stats.aof_last_rewrite_records << "\r\n";
        oss << "aof_rewrite_failures:" << stats.aof_rewrite_failures << "\r\n";
        oss << "aof_rewrite_last_status:" << stats.aof_rewrite_last_status << "\r\n";
        oss << "aof_rewrite_last_error:" << stats.aof_rewrite_last_error << "\r\n";
        oss << "\r\n";
    }

    if (wants_replication) {
        auto replicas = splitNodeList(config_.replicas_str);
        oss << "# Replication\r\n";
        oss << "role:" << (isReplica() ? "slave" : "master") << "\r\n";
        oss << "master_node:" << (isReplica() ? config_.replicaof : "") << "\r\n";
        oss << "promoted_from:" << (promoted_to_master_ ? config_.replicaof : "") << "\r\n";
        oss << "master_repl_offset:"
            << (replication_backlog_ ? replication_backlog_->currentOffset() : 0) << "\r\n";
        oss << "slave_repl_offset:"
            << (replication_offset_ ? replication_offset_->load(std::memory_order_relaxed) : 0)
            << "\r\n";
        oss << "repl_backlog_active:" << (replication_backlog_ ? "1" : "0") << "\r\n";
        oss << "repl_backlog_histlen:"
            << (replication_backlog_ ? replication_backlog_->size() : 0) << "\r\n";
        oss << "connected_slaves:" << replicas.size() << "\r\n";
        for (size_t i = 0; i < replicas.size(); ++i) {
            oss << "slave" << i << ":addr=" << replicas[i] << ",state=online\r\n";
        }
        oss << "\r\n";
    }

    if (wants_cluster) {
        auto nodes = clusterNodes();
        size_t assigned_slots = 0;
        size_t failed_nodes = 0;
        size_t suspect_nodes = 0;
        uint64_t epoch = 0;
        if (slot_map_ && slot_map_mutex_) {
            std::lock_guard<std::mutex> lock(*slot_map_mutex_);
            assigned_slots = slot_map_->AssignedSlotCount();
            failed_nodes = slot_map_->FailedNodeCount();
            suspect_nodes = slot_map_->SuspectNodeCount();
            epoch = slot_map_->GetEpoch();
        }

        oss << "# Cluster\r\n";
        oss << "cluster_enabled:" << (cluster_mode_ ? "1" : "0") << "\r\n";
        oss << "cluster_state:" << (cluster_mode_ && (nodes.empty() || failed_nodes > 0) ? "fail" : "ok") << "\r\n";
        oss << "cluster_slots_assigned:" << assigned_slots << "\r\n";
        oss << "cluster_known_nodes:" << nodes.size() << "\r\n";
        oss << "cluster_failed_nodes:" << failed_nodes << "\r\n";
        oss << "cluster_suspect_nodes:" << suspect_nodes << "\r\n";
        oss << "cluster_current_epoch:" << epoch << "\r\n";
        oss << "cluster_current_node:" << current_node_ << "\r\n";
        oss << "\r\n";
    }

    return RespWriter::bulkString(oss.str());
}

std::string CommandHandler::handleSlowLogCommand(const RespValue& cmd) const {
    if (cmd.array.size() < 2 || cmd.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'slowlog'");
    }
    std::string subcmd = toUpper(cmd.array[1].str);

    if (subcmd == "LEN") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'slowlog len'");
        }
        return RespWriter::integer(static_cast<long long>(Stats::instance().slowLogLen()));
    }

    if (subcmd == "RESET") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'slowlog reset'");
        }
        Stats::instance().resetSlowLog();
        return RespWriter::simpleString("OK");
    }

    if (subcmd == "GET") {
        if (cmd.array.size() > 3) {
            return RespWriter::error("wrong number of arguments for 'slowlog get'");
        }
        size_t count = 10;
        if (cmd.array.size() == 3) {
            if (cmd.array[2].type != RespType::BULK_STRING ||
                !parseNonNegativeSize(cmd.array[2].str, count)) {
                return RespWriter::error("invalid slowlog count");
            }
        }

        std::vector<std::string> entries;
        auto slow_entries = Stats::instance().slowLogEntries(count);
        entries.reserve(slow_entries.size());
        for (const auto& entry : slow_entries) {
            entries.push_back(slowLogEntryResponse(entry));
        }
        return respArray(entries);
    }

    return RespWriter::error("unsupported SLOWLOG subcommand '" + subcmd + "'");
}

std::vector<std::string> CommandHandler::clusterNodes() const {
    if (!cluster_mode_) {
        return {current_node_};
    }
    if (!slot_map_ || !slot_map_mutex_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(*slot_map_mutex_);
    return slot_map_->GetAllNodes();
}

void CommandHandler::replicateWrite(const std::vector<std::string>& command) const {
    uint64_t offset = 0;
    if (replication_backlog_) {
        offset = replication_backlog_->append(command);
    }

    auto replicas = splitNodeList(config_.replicas_str);
    if (replicas.empty()) return;

    std::vector<std::string> wire_command = command;
    if (offset > 0) {
        wire_command.clear();
        wire_command.reserve(command.size() + 2);
        wire_command.push_back("REPLAPPLY");
        wire_command.push_back(std::to_string(offset));
        wire_command.insert(wire_command.end(), command.begin(), command.end());
    }

    for (const auto& replica : replicas) {
        std::string response;
        if (!sendClusterCommandToNode(replica, config_.requirepass, wire_command, response) ||
            response.rfind("+OK", 0) != 0) {
            MINIREDIS_LOG_WARN("replication")
                << "failed to replicate command to " << replica;
        }
    }
}

std::string CommandHandler::handleReplicationCommand(const std::string& cmd_name, const RespValue& cmd) {
    if (cmd_name == "REPLSNAPSHOT") {
        if (cmd.array.size() != 1) {
            return RespWriter::error("wrong number of arguments for 'replsnapshot'");
        }
        refreshRuntimeStats();
        return replicationSnapshotResponse(cache_.snapshot());
    }

    if (cmd_name == "REPLFULLSYNC") {
        if (cmd.array.size() != 1) {
            return RespWriter::error("wrong number of arguments for 'replfullsync'");
        }
        refreshRuntimeStats();
        uint64_t offset = replication_backlog_ ? replication_backlog_->currentOffset() : 0;
        return replicationFullSyncResponse(offset, cache_.snapshot());
    }

    if (cmd_name == "REPLPSYNC") {
        if (cmd.array.size() != 2 || cmd.array[1].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'replpsync'");
        }
        uint64_t last_offset = 0;
        if (!parseU64Strict(cmd.array[1].str, last_offset)) {
            return RespWriter::error("invalid replication offset");
        }
        uint64_t current_offset = replication_backlog_ ? replication_backlog_->currentOffset() : 0;
        std::vector<ReplicationBacklogEntry> entries;
        if (!replication_backlog_ || !replication_backlog_->entriesAfter(last_offset, entries)) {
            return replicationFullResyncResponse(current_offset);
        }
        return replicationPsyncResponse(current_offset, entries);
    }

    if (cmd_name == "REPLAPPLY") {
        if (cmd.array.size() < 4 || cmd.array[1].type != RespType::BULK_STRING ||
            cmd.array[2].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'replapply'");
        }
        uint64_t offset = 0;
        if (!parseU64Strict(cmd.array[1].str, offset)) {
            return RespWriter::error("invalid replication offset");
        }

        RespValue inner;
        inner.type = RespType::ARRAY;
        inner.array.reserve(cmd.array.size() - 2);
        for (size_t i = 2; i < cmd.array.size(); ++i) {
            inner.array.push_back(cmd.array[i]);
        }
        std::string inner_name = toUpper(inner.array[0].str);
        if (inner_name == "REPLAPPLY" || inner_name == "REPLSNAPSHOT" ||
            inner_name == "REPLFULLSYNC" || inner_name == "REPLPSYNC") {
            return RespWriter::error("invalid nested replication command");
        }
        std::string response = handleReplicationCommand(inner_name, inner);
        if (response.rfind("+OK", 0) == 0) {
            if (replication_offset_) {
                replication_offset_->store(offset, std::memory_order_relaxed);
            }
            if (replication_offset_callback_) {
                replication_offset_callback_(offset);
            }
        }
        return response;
    }

    if (cmd_name == "REPLSET") {
        if (cmd.array.size() != 4 ||
            cmd.array[1].type != RespType::BULK_STRING ||
            cmd.array[2].type != RespType::BULK_STRING ||
            cmd.array[3].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'replset'");
        }
        std::string key_error = validateKey(cmd.array[1]);
        if (!key_error.empty()) return key_error;
        std::string value_error = validateValue(cmd.array[2]);
        if (!value_error.empty()) return value_error;
        size_t ttl_seconds_size = 0;
        if (!parseNonNegativeSize(cmd.array[3].str, ttl_seconds_size) ||
            ttl_seconds_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return RespWriter::error("invalid replication ttl");
        }
        int ttl_seconds = static_cast<int>(ttl_seconds_size);
        SetResult result = cache_.set(cmd.array[1].str, cmd.array[2].str, ttl_seconds);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        if (result != SetResult::Ok) {
            return result == SetResult::OutOfMemory
                       ? RespWriter::error("OOM maxmemory limit reached")
                       : RespWriter::error("allocation failed");
        }
        if (aof_ && !aof_->appendSet(cmd.array[1].str, cmd.array[2].str, ttl_seconds)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append replicated SET command";
        }
        return RespWriter::simpleString("OK");
    }

    if (cmd_name == "REPLDEL") {
        if (cmd.array.size() < 2) {
            return RespWriter::error("wrong number of arguments for 'repldel'");
        }
        std::vector<std::string> deleted_keys;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            if (cmd.array[i].type != RespType::BULK_STRING) {
                return RespWriter::error("key must be bulk string");
            }
            std::string key_error = validateKey(cmd.array[i]);
            if (!key_error.empty()) return key_error;
            if (cache_.del(cmd.array[i].str)) {
                deleted_keys.push_back(cmd.array[i].str);
            }
        }
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        if (aof_ && !deleted_keys.empty() && !aof_->appendDel(deleted_keys)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append replicated DEL command";
        }
        return RespWriter::simpleString("OK");
    }

    if (cmd_name == "REPLEXPIRE") {
        if (cmd.array.size() != 3 ||
            cmd.array[1].type != RespType::BULK_STRING ||
            cmd.array[2].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'replexpire'");
        }
        std::string key_error = validateKey(cmd.array[1]);
        if (!key_error.empty()) return key_error;
        int ttl_seconds = 0;
        if (!parsePositiveInt(cmd.array[2].str, ttl_seconds)) {
            return RespWriter::error("invalid expire time");
        }
        bool updated = cache_.expire(cmd.array[1].str, ttl_seconds);
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        if (aof_ && updated && !aof_->appendExpire(cmd.array[1].str, ttl_seconds)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append replicated EXPIRE command";
        }
        return RespWriter::simpleString("OK");
    }

    return RespWriter::error("unknown replication command");
}

std::string CommandHandler::handleClusterSetSlot(const RespValue& cmd) {
    if (!cluster_mode_ || !slot_map_ || !slot_map_mutex_) {
        return RespWriter::error("cluster mode is not enabled");
    }
    if (cmd.array.size() < 4 || cmd.array[2].type != RespType::BULK_STRING ||
        cmd.array[3].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'cluster setslot'");
    }

    int slot_value = 0;
    if (!parseSlot(cmd.array[2].str, slot_value)) {
        return RespWriter::error("invalid slot");
    }
    uint16_t slot = static_cast<uint16_t>(slot_value);

    std::string action = toUpper(cmd.array[3].str);
    bool ok = false;
    if (action == "STABLE") {
        if (cmd.array.size() != 4) {
            return RespWriter::error("wrong number of arguments for 'cluster setslot stable'");
        }
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        ok = slot_map_->SetSlotState(slot, ClusterSlotState::Stable);
    } else if (action == "NODE" || action == "MIGRATING" || action == "IMPORTING") {
        if (cmd.array.size() != 5 || cmd.array[4].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'cluster setslot'");
        }
        const std::string& node = cmd.array[4].str;
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        if (action == "NODE") {
            ok = slot_map_->SetSlotOwner(slot, node);
        } else if (action == "MIGRATING") {
            ok = slot_map_->SetSlotState(slot, ClusterSlotState::Migrating, node);
        } else {
            ok = slot_map_->SetSlotState(slot, ClusterSlotState::Importing, node);
        }
    } else {
        return RespWriter::error("unsupported CLUSTER SETSLOT action '" + action + "'");
    }

    if (!ok) {
        return RespWriter::error("invalid CLUSTER SETSLOT target");
    }
    if (cluster_change_callback_) cluster_change_callback_();
    return RespWriter::simpleString("OK");
}

std::string CommandHandler::handleClusterMigrate(const RespValue& cmd) {
    if (!cluster_mode_ || !slot_map_ || !slot_map_mutex_) {
        return RespWriter::error("cluster mode is not enabled");
    }
    if (cmd.array.size() != 4 || cmd.array[2].type != RespType::BULK_STRING ||
        cmd.array[3].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'cluster migrate'");
    }

    int slot_value = 0;
    if (!parseSlot(cmd.array[2].str, slot_value)) {
        return RespWriter::error("invalid slot");
    }
    uint16_t slot = static_cast<uint16_t>(slot_value);
    std::string target_node = cmd.array[3].str;

    std::vector<std::string> nodes;
    {
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        std::string owner = slot_map_->GetNodeForSlot(slot);
        if (owner != current_node_) {
            return RespWriter::error("slot is not owned by current node");
        }
        nodes = slot_map_->GetAllNodes();
        if (std::find(nodes.begin(), nodes.end(), target_node) == nodes.end() ||
            target_node == current_node_) {
            return RespWriter::error("invalid migration target node");
        }
        if (!slot_map_->SetSlotState(slot, ClusterSlotState::Migrating, target_node)) {
            return RespWriter::error("failed to mark slot migrating");
        }
    }
    if (cluster_change_callback_) cluster_change_callback_();

    std::string response;
    if (!sendClusterCommandToNode(target_node, config_.requirepass,
                                  {"CLUSTER", "SETSLOT", std::to_string(slot),
                                   "IMPORTING", current_node_},
                                  response) ||
        response.rfind("+OK", 0) != 0) {
        return RespWriter::error("failed to mark target importing");
    }

    std::vector<std::string> migrated_keys;
    for (const auto& key : cache_.keys()) {
        if (clusterHashSlot(key) != slot) continue;
        auto value = cache_.get(key);
        if (!value) continue;
        long long ttl_seconds = cache_.ttl(key);

        std::vector<std::string> set_command{"SET", key, *value};
        if (ttl_seconds >= 0) {
            set_command.push_back("EX");
            set_command.push_back(std::to_string(std::max<long long>(ttl_seconds, 1)));
        }
        if (!sendAskingCommandToNode(target_node, config_.requirepass, set_command, response) ||
            response.rfind("+OK", 0) != 0) {
            return RespWriter::error("failed to copy key '" + key + "' to target node");
        }
        migrated_keys.push_back(key);
    }

    for (const auto& node : nodes) {
        if (node == current_node_) continue;
        if (!sendClusterCommandToNode(node, config_.requirepass,
                                      {"CLUSTER", "SETSLOT", std::to_string(slot),
                                       "NODE", target_node},
                                      response) ||
            response.rfind("+OK", 0) != 0) {
            return RespWriter::error("failed to update slot owner on node '" + node + "'");
        }
    }

    std::vector<std::string> deleted_keys;
    for (const auto& key : migrated_keys) {
        if (cache_.del(key)) deleted_keys.push_back(key);
    }
    if (aof_ && !deleted_keys.empty() && !aof_->appendDel(deleted_keys)) {
        MINIREDIS_LOG_ERROR("aof") << "failed to append migrated DEL command";
    }
    {
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        if (!slot_map_->SetSlotOwner(slot, target_node)) {
            return RespWriter::error("failed to update local slot owner");
        }
    }
    if (cluster_change_callback_) cluster_change_callback_();
    refreshRuntimeStats();
    return RespWriter::integer(static_cast<long long>(migrated_keys.size()));
}

std::string CommandHandler::handleClusterCommand(const RespValue& cmd) {
    if (cmd.array.size() < 2 || cmd.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'cluster'");
    }

    std::string subcmd = cmd.array[1].str;
    subcmd = toUpper(std::move(subcmd));

    if (subcmd == "KEYSLOT") {
        if (cmd.array.size() != 3 || cmd.array[2].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'cluster keyslot'");
        }
        std::string key_error = validateKey(cmd.array[2]);
        if (!key_error.empty()) return key_error;
        return RespWriter::integer(clusterHashSlot(cmd.array[2].str));
    }

    if (subcmd == "MYID") {
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'cluster myid'");
        }
        return RespWriter::bulkString(clusterNodeId(current_node_));
    }

    if (subcmd == "COUNTKEYSINSLOT") {
        if (cmd.array.size() != 3 || cmd.array[2].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'cluster countkeysinslot'");
        }
        int slot = 0;
        if (!parseSlot(cmd.array[2].str, slot)) {
            return RespWriter::error("invalid slot");
        }
        long long count = 0;
        for (const auto& key : cache_.keys()) {
            if (clusterHashSlot(key) == static_cast<uint16_t>(slot)) {
                ++count;
            }
        }
        return RespWriter::integer(count);
    }

    if (subcmd == "SETSLOT") {
        return handleClusterSetSlot(cmd);
    }

    if (subcmd == "MIGRATE") {
        return handleClusterMigrate(cmd);
    }

    if (subcmd == "FAILOVER") {
        return handleClusterFailover(cmd);
    }

    if (subcmd == "INFO") {
        auto nodes = clusterNodes();
        size_t assigned_slots = 0;
        size_t failed_nodes = 0;
        size_t suspect_nodes = 0;
        uint64_t epoch = 0;
        if (slot_map_ && slot_map_mutex_) {
            std::lock_guard<std::mutex> lock(*slot_map_mutex_);
            assigned_slots = slot_map_->AssignedSlotCount();
            failed_nodes = slot_map_->FailedNodeCount();
            suspect_nodes = slot_map_->SuspectNodeCount();
            epoch = slot_map_->GetEpoch();
        }
        std::ostringstream oss;
        oss << "cluster_enabled:" << (cluster_mode_ ? "1" : "0") << "\r\n";
        oss << "cluster_state:" << (cluster_mode_ && (nodes.empty() || failed_nodes > 0) ? "fail" : "ok") << "\r\n";
        oss << "cluster_slots_assigned:" << assigned_slots << "\r\n";
        oss << "cluster_known_nodes:" << nodes.size() << "\r\n";
        oss << "cluster_failed_nodes:" << failed_nodes << "\r\n";
        oss << "cluster_suspect_nodes:" << suspect_nodes << "\r\n";
        oss << "cluster_current_epoch:" << epoch << "\r\n";
        oss << "cluster_current_node:" << current_node_ << "\r\n";
        //Redis 的很多 info 类命令会返回一整段文本，所以用 bulk string 很合适。
        //$96\r\ncluster_enabled:1\r\ncluster_state:ok\r\ncluster_known_nodes:2\r\ncluster_current_node:127.0.0.1:6366\r\n\r\n
        return RespWriter::bulkString(oss.str());
    }

    if (subcmd == "NODES") {
        auto nodes = clusterNodes();
        if (cluster_mode_ && nodes.empty()) {
            return RespWriter::error("cluster slot map not initialized");
        }

        std::ostringstream oss;
        for (const auto& node : nodes) {
            bool myself = (node == current_node_);
            ClusterNodeState state = ClusterNodeState::Healthy;
            if (slot_map_ && slot_map_mutex_) {
                std::lock_guard<std::mutex> lock(*slot_map_mutex_);
                state = slot_map_->GetNodeState(node);
            }
            std::string flags = myself
                                    ? (isReplica() ? "myself,slave" : "myself,master")
                                    : "master";
            if (state == ClusterNodeState::Suspect) {
                flags += ",pfail";
            } else if (state == ClusterNodeState::Fail) {
                flags += ",fail";
            }
            oss << clusterNodeId(node) << " "
                << node << " "
                << flags
                << " - 0 0 0 " << (state == ClusterNodeState::Fail ? "disconnected" : "connected");
            if (slot_map_ && slot_map_mutex_) {
                std::lock_guard<std::mutex> lock(*slot_map_mutex_);
                for (const auto& range : slot_map_->GetSlotRangesForNode(node)) {
                    oss << " " << range.start;
                    if (range.start != range.end) {
                        oss << "-" << range.end;
                    }
                }
                for (uint16_t slot = 0; slot < kRedisClusterSlots; ++slot) {
                    ClusterSlotMeta meta = slot_map_->GetSlotMeta(slot);
                    if (meta.state == ClusterSlotState::Migrating &&
                        slot_map_->GetNodeForSlot(slot) == node) {
                        oss << " [" << slot << "->-" << clusterNodeId(meta.peer_node) << "]";
                    } else if (meta.state == ClusterSlotState::Importing &&
                               node == current_node_) {
                        oss << " [" << slot << "-<-" << clusterNodeId(meta.peer_node) << "]";
                    }
                }
            }
            oss << "\n";
        }
        return RespWriter::bulkString(oss.str());
    }

    if (subcmd == "SLOTS") {
        auto nodes = clusterNodes();
        if (cluster_mode_ && nodes.empty()) {
            return RespWriter::error("cluster slot map not initialized");
        }

        std::vector<std::string> slots;
        if (slot_map_ && slot_map_mutex_) {
            std::lock_guard<std::mutex> lock(*slot_map_mutex_);
            for (const auto& node : nodes) {
                std::string host;
                int port = 0;
                if (!splitNodeAddr(node, host, port)) continue;

                for (const auto& range : slot_map_->GetSlotRangesForNode(node)) {
                    std::string node_info = respArray({
                        RespWriter::bulkString(host),
                        RespWriter::integer(port),
                        RespWriter::bulkString(clusterNodeId(node)),
                    });
                    slots.push_back(respArray({
                        RespWriter::integer(range.start),
                        RespWriter::integer(range.end),
                        node_info,
                    }));
                }
            }
        }
        return respArray(slots);
    }

    if (subcmd == "SLOTMAP") {
        if (!cluster_mode_ || !slot_map_ || !slot_map_mutex_) {
            return RespWriter::error("cluster mode is not enabled");
        }
        if (cmd.array.size() != 2) {
            return RespWriter::error("wrong number of arguments for 'cluster slotmap'");
        }
        ClusterSlotMapSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(*slot_map_mutex_);
            snapshot = slot_map_->ExportSnapshot();
        }
        return clusterSlotMapResponse(snapshot);
    }

    return RespWriter::error("unsupported CLUSTER subcommand '" + subcmd + "'");
}

std::string CommandHandler::handleClusterFailover(const RespValue& cmd) {
    if (cmd.array.size() > 3) {
        return RespWriter::error("wrong number of arguments for 'cluster failover'");
    }
    if (cmd.array.size() == 3) {
        if (cmd.array[2].type != RespType::BULK_STRING ||
            toUpper(cmd.array[2].str) != "TAKEOVER") {
            return RespWriter::error("unsupported CLUSTER FAILOVER mode");
        }
    }
    if (!cluster_mode_ || !slot_map_ || !slot_map_mutex_) {
        return RespWriter::error("cluster slot map not initialized");
    }
    if (config_.replicaof.empty()) {
        return RespWriter::error("CLUSTER FAILOVER is only supported on replicas");
    }
    if (promoted_to_master_) {
        return RespWriter::simpleString("OK");
    }

    size_t moved_slots = 0;
    {
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        auto nodes = slot_map_->GetAllNodes();
        if (std::find(nodes.begin(), nodes.end(), current_node_) == nodes.end()) {
            return RespWriter::error("current node is not in cluster slot map");
        }
        if (std::find(nodes.begin(), nodes.end(), config_.replicaof) == nodes.end()) {
            return RespWriter::error("master node is not in cluster slot map");
        }

        for (uint16_t slot = 0; slot < kRedisClusterSlots; ++slot) {
            if (slot_map_->GetNodeForSlot(slot) == config_.replicaof) {
                if (!slot_map_->SetSlotOwner(slot, current_node_)) {
                    return RespWriter::error("failed to take over slot " + std::to_string(slot));
                }
                ++moved_slots;
            }
        }
        slot_map_->MarkNodeFailed(config_.replicaof);
        slot_map_->MarkNodeHealthy(current_node_);
    }

    promoted_to_master_ = true;
    if (cluster_change_callback_) cluster_change_callback_();
    MINIREDIS_LOG_WARN("cluster") << "manual failover promoted " << current_node_
                                  << " from master " << config_.replicaof
                                  << ", took over " << moved_slots << " slots";
    return RespWriter::simpleString("OK");
}

std::string CommandHandler::handle(const RespValue& cmd, bool& authenticated) {
    CommandSession session;
    session.authenticated = authenticated || !authRequired();
    session.role = AclRole::Admin;
    std::string response = handle(cmd, session);
    authenticated = session.authenticated;
    return response;
}

std::string CommandHandler::handle(const RespValue& cmd, CommandSession& session) {
    if (cmd.type != RespType::ARRAY || cmd.array.empty()) {
        return RespWriter::error("invalid command format");
    }
    const RespValue& first = cmd.array[0];
    if (first.type != RespType::BULK_STRING && first.type != RespType::SIMPLE_STRING) {
        return RespWriter::error("command name must be string");
    }

    std::string cmd_name = first.str;
    cmd_name = toUpper(std::move(cmd_name));

    if (cmd_name == "AUTH") {
        Stats::instance().recordCommand(cmd_name);
        if (!authRequired()) {
            session.authenticated = true;
            session.role = AclRole::Admin;
            return RespWriter::error("AUTH is not required");
        }
        const bool valid_shape =
            (cmd.array.size() == 2 && cmd.array[1].type == RespType::BULK_STRING) ||
            (cmd.array.size() == 3 && cmd.array[1].type == RespType::BULK_STRING &&
             cmd.array[2].type == RespType::BULK_STRING);
        if (!valid_shape) {
            return RespWriter::error("wrong number of arguments for 'auth'");
        }
        if (authenticate(cmd, session)) {
            return RespWriter::simpleString("OK");
        }
        return RespWriter::error("invalid password");
    }

    if (!session.authenticated) {
        Stats::instance().recordCommand(cmd_name);
        return RespWriter::error("NOAUTH Authentication required");
    }

    if (cmd_name == "ACL") {
        Stats::instance().recordCommand(cmd_name);
        return handleAclCommand(cmd, session);
    }

    if (!isAllowed(cmd_name, session.role)) {
        Stats::instance().recordCommand(cmd_name);
        return RespWriter::error("NOPERM this user has no permissions to run the command");
    }

    if (cmd_name == "ASKING") {
        Stats::instance().recordCommand(cmd_name);
        if (cmd.array.size() != 1) {
            return RespWriter::error("wrong number of arguments for 'asking'");
        }
        session.asking = true;
        return RespWriter::simpleString("OK");
    }

    if (cmd_name == "REPLSET" || cmd_name == "REPLDEL" ||
        cmd_name == "REPLEXPIRE" || cmd_name == "REPLSNAPSHOT" ||
        cmd_name == "REPLFULLSYNC" || cmd_name == "REPLPSYNC" ||
        cmd_name == "REPLAPPLY") {
        Stats::instance().recordCommand(cmd_name);
        return handleReplicationCommand(cmd_name, cmd);
    }

    if (isReplica() && isClientWriteCommand(cmd_name)) {
        Stats::instance().recordCommand(cmd_name);
        return RespWriter::error("READONLY You can't write against a read only replica");
    }

    if (cmd_name == "CLUSTER") {
        Stats::instance().recordCommand(cmd_name);
        return handleClusterCommand(cmd);
    }

    std::string route_response = routeIfNeeded(cmd_name, cmd, session);
    if (!route_response.empty()) {
        Stats::instance().recordCommand(cmd_name);
        return route_response;
    }

    if (cmd_name == "GET") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'get'");
        const RespValue& key_arg = cmd.array[1];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        auto val = cache_.get(key_arg.str);
        refreshRuntimeStats();
        if (val) {
            Stats::instance().recordGetHit();
            return RespWriter::bulkString(*val);
        }
        Stats::instance().recordGetMiss();
        return RespWriter::nullBulkString();
    }

    if (cmd_name == "STRLEN") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'strlen'");
        const RespValue& key_arg = cmd.array[1];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        auto val = cache_.get(key_arg.str);
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return RespWriter::integer(static_cast<long long>(val ? val->size() : 0));
    }

    if (cmd_name == "MGET") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'mget'");
        std::vector<std::string> values;
        values.reserve(cmd.array.size() - 1);
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            const RespValue& key_arg = cmd.array[i];
            std::string key_error = validateKey(key_arg);
            if (!key_error.empty()) return key_error;
            auto val = cache_.get(key_arg.str);
            values.push_back(val ? RespWriter::bulkString(*val) : RespWriter::nullBulkString());
        }
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return respArray(values);
    }

    if (cmd_name == "SET") {
        if (cmd.array.size() != 3 && cmd.array.size() != 5) {
            return RespWriter::error("wrong number of arguments for 'set'");
        }
        const RespValue& key_arg = cmd.array[1];
        const RespValue& val_arg = cmd.array[2];
        if (key_arg.type != RespType::BULK_STRING || val_arg.type != RespType::BULK_STRING) {
            return RespWriter::error("key and value must be bulk strings");
        }
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        std::string value_error = validateValue(val_arg);
        if (!value_error.empty()) return value_error;
        int ttl_seconds = 0;
        if (cmd.array.size() == 5) {
            if (cmd.array[3].type != RespType::BULK_STRING ||
                cmd.array[4].type != RespType::BULK_STRING ||
                toUpper(cmd.array[3].str) != "EX" ||
                !parsePositiveInt(cmd.array[4].str, ttl_seconds)) {
                return RespWriter::error("syntax error");
            }
        }
SetResult set_result = cache_.set(key_arg.str, val_arg.str, ttl_seconds);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        if (set_result == SetResult::Ok) {
            if (aof_ && !aof_->appendSet(key_arg.str, val_arg.str, ttl_seconds)) {
                MINIREDIS_LOG_ERROR("aof") << "failed to append SET command";
            }
            replicateWrite({"REPLSET", key_arg.str, val_arg.str, std::to_string(ttl_seconds)});
            return RespWriter::simpleString("OK");
        }
        if (set_result == SetResult::OutOfMemory) {
            return RespWriter::error("OOM maxmemory limit reached");
        }
        return RespWriter::error("allocation failed");
    }

    if (cmd_name == "DEL") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'del'");
        int deleted = 0;
        std::vector<std::string> deleted_keys;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            std::string key_error = validateKey(cmd.array[i]);
            if (!key_error.empty()) return key_error;
            if (cache_.del(cmd.array[i].str)) {
                ++deleted;
                deleted_keys.push_back(cmd.array[i].str);
            }
        }
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        if (aof_ && !deleted_keys.empty() && !aof_->appendDel(deleted_keys)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append DEL command";
        }
        if (!deleted_keys.empty()) {
            std::vector<std::string> repl{"REPLDEL"};
            repl.insert(repl.end(), deleted_keys.begin(), deleted_keys.end());
            replicateWrite(repl);
        }
        return RespWriter::integer(deleted);
    }

    if (cmd_name == "SETNX") {
        if (cmd.array.size() != 3) {
            return RespWriter::error("wrong number of arguments for 'setnx'");
        }
        const RespValue& key_arg = cmd.array[1];
        const RespValue& val_arg = cmd.array[2];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        std::string value_error = validateValue(val_arg);
        if (!value_error.empty()) return value_error;

        ValueUpdateResult result = cache_.set_if_absent(key_arg.str, val_arg.str);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        if (result.status == SetResult::OutOfMemory) {
            return RespWriter::error("OOM maxmemory limit reached");
        }
        if (result.status == SetResult::AllocationFailed) {
            return RespWriter::error("allocation failed");
        }
        if (result.changed) {
            if (aof_ && !aof_->appendSet(key_arg.str, result.value, result.ttl_seconds)) {
                MINIREDIS_LOG_ERROR("aof") << "failed to append SETNX command as SET";
            }
            replicateWrite({"REPLSET", key_arg.str, result.value, std::to_string(result.ttl_seconds)});
        }
        return RespWriter::integer(result.changed ? 1 : 0);
    }

    if (cmd_name == "APPEND") {
        if (cmd.array.size() != 3) {
            return RespWriter::error("wrong number of arguments for 'append'");
        }
        const RespValue& key_arg = cmd.array[1];
        const RespValue& suffix_arg = cmd.array[2];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        std::string value_error = validateValue(suffix_arg);
        if (!value_error.empty()) return value_error;

        ValueUpdateResult result = cache_.append(key_arg.str, suffix_arg.str);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        if (result.status == SetResult::OutOfMemory) {
            return RespWriter::error("OOM maxmemory limit reached");
        }
        if (result.status == SetResult::AllocationFailed) {
            return RespWriter::error("allocation failed");
        }
        if (aof_ && !aof_->appendSet(key_arg.str, result.value, result.ttl_seconds)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append APPEND command as SET";
        }
        replicateWrite({"REPLSET", key_arg.str, result.value, std::to_string(result.ttl_seconds)});
        return RespWriter::integer(static_cast<long long>(result.length));
    }

    if (cmd_name == "INCR" || cmd_name == "DECR" ||
        cmd_name == "INCRBY" || cmd_name == "DECRBY") {
        const bool by_command = (cmd_name == "INCRBY" || cmd_name == "DECRBY");
        if ((!by_command && cmd.array.size() != 2) || (by_command && cmd.array.size() != 3)) {
            std::string lower = cmd_name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return RespWriter::error("wrong number of arguments for '" + lower + "'");
        }
        const RespValue& key_arg = cmd.array[1];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;

        long long delta = (cmd_name == "INCR" || cmd_name == "INCRBY") ? 1 : -1;
        if (by_command) {
            const RespValue& delta_arg = cmd.array[2];
            if (delta_arg.type != RespType::BULK_STRING ||
                !parseInt64Strict(delta_arg.str, delta)) {
                return RespWriter::error("ERR value is not an integer or out of range");
            }
            if (cmd_name == "DECRBY") {
                if (delta == std::numeric_limits<long long>::min()) {
                    return RespWriter::error("ERR increment or decrement would overflow");
                }
                delta = -delta;
            }
        }

        IncrementResult result = cache_.increment(key_arg.str, delta);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        if (result.code == IncrementResultCode::NotInteger) {
            return RespWriter::error("ERR value is not an integer or out of range");
        }
        if (result.code == IncrementResultCode::Overflow) {
            return RespWriter::error("ERR increment or decrement would overflow");
        }
        if (result.code == IncrementResultCode::OutOfMemory) {
            return RespWriter::error("OOM maxmemory limit reached");
        }
        if (result.code == IncrementResultCode::AllocationFailed) {
            return RespWriter::error("allocation failed");
        }
        if (aof_ && !aof_->appendSet(key_arg.str, result.value, result.ttl_seconds)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append integer command as SET";
        }
        replicateWrite({"REPLSET", key_arg.str, result.value, std::to_string(result.ttl_seconds)});
        return RespWriter::integer(result.number);
    }

    if (cmd_name == "EXISTS") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'exists'");
        int count = 0;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            std::string key_error = validateKey(cmd.array[i]);
            if (!key_error.empty()) return key_error;
            if (cache_.exists(cmd.array[i].str)) {
                ++count;
            }
        }
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return RespWriter::integer(count);
    }

    if (cmd_name == "EXPIRE") {
        if (cmd.array.size() != 3) return RespWriter::error("wrong number of arguments for 'expire'");
        const RespValue& key_arg = cmd.array[1];
        const RespValue& ttl_arg = cmd.array[2];
        if (key_arg.type != RespType::BULK_STRING || ttl_arg.type != RespType::BULK_STRING) {
            return RespWriter::error("key and ttl must be bulk strings");
        }
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        int ttl_seconds = 0;
        if (!parsePositiveInt(ttl_arg.str, ttl_seconds)) {
            return RespWriter::error("invalid expire time");
        }
        bool updated = cache_.expire(key_arg.str, ttl_seconds);
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        if (aof_ && updated && !aof_->appendExpire(key_arg.str, ttl_seconds)) {
            MINIREDIS_LOG_ERROR("aof") << "failed to append EXPIRE command";
        }
        if (updated) {
            replicateWrite({"REPLEXPIRE", key_arg.str, std::to_string(ttl_seconds)});
        }
        return RespWriter::integer(updated ? 1 : 0);
    }

    if (cmd_name == "TTL") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'ttl'");
        const RespValue& key_arg = cmd.array[1];
        std::string key_error = validateKey(key_arg);
        if (!key_error.empty()) return key_error;
        long long remaining = cache_.ttl(key_arg.str);
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return RespWriter::integer(remaining);
    }

    if (cmd_name == "PING") {
        Stats::instance().recordCommand(cmd_name);
        if (cmd.array.size() == 1) return RespWriter::simpleString("PONG");
        if (cmd.array.size() == 2 && cmd.array[1].type == RespType::BULK_STRING) {
            return RespWriter::bulkString(cmd.array[1].str);
        }
        return RespWriter::error("invalid PING arguments");
    }

    if (cmd_name == "INFO") {
        Stats::instance().recordCommand(cmd_name);
        return handleInfoCommand(cmd);
    }

    if (cmd_name == "SLOWLOG") {
        Stats::instance().recordCommand(cmd_name);
        return handleSlowLogCommand(cmd);
    }

    if (cmd_name == "BGREWRITEAOF") {
        Stats::instance().recordCommand(cmd_name);
        if (cmd.array.size() != 1) {
            return RespWriter::error("wrong number of arguments for 'bgrewriteaof'");
        }
        if (!aof_) {
            return RespWriter::error("AOF is not enabled");
        }
        refreshRuntimeStats();
        if (!aof_->rewrite(cache_.snapshot())) {
            return RespWriter::error("AOF rewrite already running or failed to start");
        }
        return RespWriter::simpleString("Background append only file rewriting started");
    }

    if (cmd_name == "COMMAND") {
        Stats::instance().recordCommand(cmd_name);
        if (cmd.array.size() == 1) {
            return commandTableResponse();
        }
        if (cmd.array.size() == 2 && cmd.array[1].type == RespType::BULK_STRING) {
            std::string subcmd = cmd.array[1].str;
            subcmd = toUpper(std::move(subcmd));
            if (subcmd == "COUNT") {
                return RespWriter::integer(static_cast<long long>(commandTable().size()));
            }
        }
        return RespWriter::error("unsupported COMMAND subcommand");
    }

    Stats::instance().recordCommand(cmd_name);
    return RespWriter::error("unknown command '" + cmd_name + "'");
}

} // namespace miniredis
