#include "miniredis/server/command_handler.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/metrics/stats.hpp"
#include <charconv>
#include <cctype>
#include <sstream>
#include <utility>
#include <vector>

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
        {"set",   -3, {"write", "denyoom"},      1,  1, 1},
        {"get",    2, {"readonly", "fast"},      1,  1, 1},
        {"del",   -2, {"write"},                 1, -1, 1},
        {"exists",-2, {"readonly", "fast"},      1, -1, 1},
        {"mget",  -2, {"readonly", "fast"},      1, -1, 1},
        {"expire", 3, {"write", "fast"},         1,  1, 1},
        {"ttl",    2, {"readonly", "fast"},      1,  1, 1},
        {"command",-1, {"loading", "stale"},     0,  0, 0},
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

bool splitNodeAddr(const std::string& node, std::string& host, int& port) {
    size_t pos = node.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= node.size()) return false;
    host = node.substr(0, pos);
    auto port_text = node.substr(pos + 1);
    auto [ptr, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    return ec == std::errc() && ptr == port_text.data() + port_text.size() &&
           port > 0 && port <= 65535;
}

} // namespace

CommandHandler::CommandHandler(CacheStore& cache, FixedMemoryPool& memory_pool,
                               const AppConfig& config, bool cluster_mode,
                               std::string current_node,
                               ClusterSlotMap* slot_map,
                               std::mutex* slot_map_mutex)
    : cache_(cache),
      memory_pool_(memory_pool),
      config_(config),
      cluster_mode_(cluster_mode),
      current_node_(std::move(current_node)),
      slot_map_(slot_map),
      slot_map_mutex_(slot_map_mutex) {}

void CommandHandler::refreshRuntimeStats() const {
    Stats::instance().setKeyCount(cache_.key_count());
    Stats::instance().setMemoryPoolUsed(memory_pool_.used_blocks(), memory_pool_.free_blocks());
}

std::string CommandHandler::routeIfNeeded(const std::string& cmd_name, const RespValue& cmd) const {
    bool needs_route = (cmd_name == "GET" || cmd_name == "SET" ||
                        cmd_name == "DEL" || cmd_name == "EXISTS" ||
                        cmd_name == "MGET" || cmd_name == "EXPIRE" ||
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
        if (key_arg.type != RespType::BULK_STRING) {
            return RespWriter::error("key must be bulk string");
        }
        uint16_t key_slot = clusterHashSlot(key_arg.str);
        if (!saw_key) {
            slot = key_slot;
            saw_key = true;
        } else if (key_slot != slot) {
            return RespWriter::error("CROSSSLOT Keys in request don't hash to the same slot");
        }
    }

    std::string target;
    {
        std::lock_guard<std::mutex> lock(*slot_map_mutex_);
        target = slot_map_->GetNodeForSlot(slot);
    }
    if (target.empty()) {
        return RespWriter::error("cluster has no active nodes");
    }
    if (target != current_node_) {
        return "-MOVED " + std::to_string(slot) + " " + target + "\r\n";
    }
    return {};
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

std::string CommandHandler::handleClusterCommand(const RespValue& cmd) const {
    if (cmd.array.size() < 2 || cmd.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'cluster'");
    }

    std::string subcmd = cmd.array[1].str;
    subcmd = toUpper(std::move(subcmd));

    if (subcmd == "KEYSLOT") {
        if (cmd.array.size() != 3 || cmd.array[2].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'cluster keyslot'");
        }
        return RespWriter::integer(clusterHashSlot(cmd.array[2].str));
    }

    if (subcmd == "INFO") {
        auto nodes = clusterNodes();
        std::ostringstream oss;
        oss << "cluster_enabled:" << (cluster_mode_ ? "1" : "0") << "\r\n";
        oss << "cluster_state:" << (cluster_mode_ && nodes.empty() ? "fail" : "ok") << "\r\n";
        oss << "cluster_known_nodes:" << nodes.size() << "\r\n";
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
            oss << clusterNodeId(node) << " "
                << node << " "
                << (myself ? "myself,master" : "master")
                << " - 0 0 0 connected";
            if (slot_map_ && slot_map_mutex_) {
                std::lock_guard<std::mutex> lock(*slot_map_mutex_);
                for (const auto& range : slot_map_->GetSlotRangesForNode(node)) {
                    oss << " " << range.start;
                    if (range.start != range.end) {
                        oss << "-" << range.end;
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

    return RespWriter::error("unsupported CLUSTER subcommand '" + subcmd + "'");
}

std::string CommandHandler::handle(const RespValue& cmd, bool& authenticated) {
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
        if (config_.requirepass.empty()) {
            authenticated = true;
            return RespWriter::error("AUTH is not required");
        }
        if (cmd.array.size() != 2 || cmd.array[1].type != RespType::BULK_STRING) {
            return RespWriter::error("wrong number of arguments for 'auth'");
        }
        if (cmd.array[1].str == config_.requirepass) {
            authenticated = true;
            return RespWriter::simpleString("OK");
        }
        return RespWriter::error("invalid password");
    }

    if (!authenticated) {
        Stats::instance().recordCommand(cmd_name);
        return RespWriter::error("NOAUTH Authentication required");
    }

    if (cmd_name == "CLUSTER") {
        Stats::instance().recordCommand(cmd_name);
        return handleClusterCommand(cmd);
    }

    std::string route_response = routeIfNeeded(cmd_name, cmd);
    if (!route_response.empty()) {
        Stats::instance().recordCommand(cmd_name);
        return route_response;
    }

    if (cmd_name == "GET") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'get'");
        const RespValue& key_arg = cmd.array[1];
        if (key_arg.type != RespType::BULK_STRING) return RespWriter::error("key must be bulk string");
        auto val = cache_.get(key_arg.str);
        refreshRuntimeStats();
        if (val) {
            Stats::instance().recordGetHit();
            return RespWriter::bulkString(*val);
        }
        Stats::instance().recordGetMiss();
        return RespWriter::nullBulkString();
    }

    if (cmd_name == "MGET") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'mget'");
        std::vector<std::string> values;
        values.reserve(cmd.array.size() - 1);
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            const RespValue& key_arg = cmd.array[i];
            if (key_arg.type != RespType::BULK_STRING) return RespWriter::error("key must be bulk string");
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
        int ttl_seconds = 0;
        if (cmd.array.size() == 5) {
            if (cmd.array[3].type != RespType::BULK_STRING ||
                cmd.array[4].type != RespType::BULK_STRING ||
                toUpper(cmd.array[3].str) != "EX" ||
                !parsePositiveInt(cmd.array[4].str, ttl_seconds)) {
                return RespWriter::error("syntax error");
            }
        }
        cache_.set(key_arg.str, val_arg.str, ttl_seconds);
        Stats::instance().recordSet();
        refreshRuntimeStats();
        return RespWriter::simpleString("OK");
    }

    if (cmd_name == "DEL") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'del'");
        int deleted = 0;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            if (cmd.array[i].type == RespType::BULK_STRING && cache_.del(cmd.array[i].str)) {
                ++deleted;
            }
        }
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return RespWriter::integer(deleted);
    }

    if (cmd_name == "EXISTS") {
        if (cmd.array.size() < 2) return RespWriter::error("wrong number of arguments for 'exists'");
        int count = 0;
        for (size_t i = 1; i < cmd.array.size(); ++i) {
            if (cmd.array[i].type == RespType::BULK_STRING && cache_.exists(cmd.array[i].str)) {
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
        int ttl_seconds = 0;
        if (!parsePositiveInt(ttl_arg.str, ttl_seconds)) {
            return RespWriter::error("invalid expire time");
        }
        bool updated = cache_.expire(key_arg.str, ttl_seconds);
        Stats::instance().recordCommand(cmd_name);
        refreshRuntimeStats();
        return RespWriter::integer(updated ? 1 : 0);
    }

    if (cmd_name == "TTL") {
        if (cmd.array.size() != 2) return RespWriter::error("wrong number of arguments for 'ttl'");
        const RespValue& key_arg = cmd.array[1];
        if (key_arg.type != RespType::BULK_STRING) return RespWriter::error("key must be bulk string");
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
