#include "miniredis/server/command_handler.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/metrics/stats.hpp"
#include <cctype>
#include <sstream>
#include <utility>

namespace miniredis {

CommandHandler::CommandHandler(CacheStore& cache, FixedMemoryPool& memory_pool,
                               const AppConfig& config, bool cluster_mode,
                               std::string current_node,
                               ConsistentHash* hash_ring,
                               std::mutex* hash_ring_mutex)
    : cache_(cache),
      memory_pool_(memory_pool),
      config_(config),
      cluster_mode_(cluster_mode),
      current_node_(std::move(current_node)),
      hash_ring_(hash_ring),
      hash_ring_mutex_(hash_ring_mutex) {}

void CommandHandler::refreshRuntimeStats() const {
    Stats::instance().setKeyCount(cache_.key_count());
    Stats::instance().setMemoryPoolUsed(memory_pool_.used_blocks(), memory_pool_.free_blocks());
}

std::string CommandHandler::routeIfNeeded(const std::string& cmd_name, const RespValue& cmd) const {
    bool needs_route = (cmd_name == "GET" || cmd_name == "SET" ||
                        cmd_name == "DEL" || cmd_name == "EXISTS");
    if (!needs_route || !cluster_mode_) return {};
    if (cmd.array.size() < 2) {
        return RespWriter::error("wrong number of arguments for '" + cmd_name + "'");
    }
    const RespValue& key_arg = cmd.array[1];
    if (key_arg.type != RespType::BULK_STRING) {
        return RespWriter::error("key must be bulk string");
    }
    if (!hash_ring_ || !hash_ring_mutex_) {
        return RespWriter::error("cluster ring not initialized");
    }

    std::string target;
    {
        std::lock_guard<std::mutex> lock(*hash_ring_mutex_);
        target = hash_ring_->GetNode(key_arg.str);
    }
    if (target.empty()) {
        return RespWriter::error("cluster has no active nodes");
    }
    if (target != current_node_) {
        return "-MOVED " + std::to_string(clusterHashSlot(key_arg.str)) + " " + target + "\r\n";
    }
    return {};
}

std::vector<std::string> CommandHandler::clusterNodes() const {
    if (!cluster_mode_) {
        return {current_node_};
    }
    if (!hash_ring_ || !hash_ring_mutex_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(*hash_ring_mutex_);
    return hash_ring_->GetAllNodes();
}

std::string CommandHandler::handleClusterCommand(const RespValue& cmd) const {
    if (cmd.array.size() < 2 || cmd.array[1].type != RespType::BULK_STRING) {
        return RespWriter::error("wrong number of arguments for 'cluster'");
    }

    std::string subcmd = cmd.array[1].str;
    for (char& c : subcmd) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

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
        return RespWriter::bulkString(oss.str());
    }

    if (subcmd == "NODES") {
        auto nodes = clusterNodes();
        if (cluster_mode_ && nodes.empty()) {
            return RespWriter::error("cluster ring not initialized");
        }

        std::ostringstream oss;
        for (const auto& node : nodes) {
            bool myself = (node == current_node_);
            oss << clusterNodeId(node) << " "
                << node << " "
                << (myself ? "myself,master" : "master")
                << " - 0 0 0 connected\n";
        }
        return RespWriter::bulkString(oss.str());
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
    for (char& c : cmd_name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

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
    if (!route_response.empty()) return route_response;

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

    if (cmd_name == "SET") {
        if (cmd.array.size() != 3) return RespWriter::error("wrong number of arguments for 'set'");
        const RespValue& key_arg = cmd.array[1];
        const RespValue& val_arg = cmd.array[2];
        if (key_arg.type != RespType::BULK_STRING || val_arg.type != RespType::BULK_STRING) {
            return RespWriter::error("key and value must be bulk strings");
        }
        cache_.set(key_arg.str, val_arg.str);
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
        return RespWriter::array({});
    }

    Stats::instance().recordCommand(cmd_name);
    return RespWriter::error("unknown command '" + cmd_name + "'");
}

} // namespace miniredis
