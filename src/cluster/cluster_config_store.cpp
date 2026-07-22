#include "miniredis/cluster/cluster_config_store.hpp"
#include "miniredis/core/logger.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace miniredis {
namespace {

constexpr const char* kClusterConfigMagic = "MINIREDIS_CLUSTER_CONFIG_V1";
constexpr size_t kMaxClusterNodes = 1024;

bool fsyncPath(const std::string& path, bool directory) {
    int flags = O_RDONLY;
    if (directory) flags |= O_DIRECTORY;
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

bool writeSnapshot(std::ostream& out, const ClusterSlotMapSnapshot& snapshot) {
    out << kClusterConfigMagic << '\n';
    out << "epoch " << snapshot.epoch << '\n';
    out << "nodes " << snapshot.nodes.size() << '\n';
    for (const auto& node : snapshot.nodes) {
        auto it = snapshot.node_states.find(node);
        ClusterNodeState state = (it == snapshot.node_states.end())
                                     ? ClusterNodeState::Healthy
                                     : it->second;
        out << "node " << node << " " << clusterNodeStateName(state) << '\n';
    }

    std::vector<std::tuple<uint16_t, uint16_t, std::string>> ranges;
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
            ranges.emplace_back(start, static_cast<uint16_t>(slot - 1), owner);
            in_range = false;
            if (!current.empty()) {
                in_range = true;
                start = slot;
                owner = current;
            }
        }
    }
    if (in_range) {
        ranges.emplace_back(start, static_cast<uint16_t>(kRedisClusterSlots - 1), owner);
    }

    out << "slots " << ranges.size() << '\n';
    for (const auto& [start_slot, end_slot, node] : ranges) {
        out << "slot " << start_slot << " " << end_slot << " " << node << '\n';
    }

    size_t slot_state_count = 0;
    for (const auto& meta : snapshot.slot_meta) {
        if (meta.state != ClusterSlotState::Stable) ++slot_state_count;
    }
    out << "slotstates " << slot_state_count << '\n';
    for (size_t slot = 0; slot < snapshot.slot_meta.size(); ++slot) {
        const auto& meta = snapshot.slot_meta[slot];
        if (meta.state == ClusterSlotState::Stable) continue;
        out << "slotstate " << slot << " " << clusterSlotStateName(meta.state)
            << " " << meta.peer_node << '\n';
    }
    return static_cast<bool>(out);
}

} // namespace

bool saveClusterConfig(const std::string& path, const ClusterSlotMap& slot_map) {
    if (path.empty()) return true;

    std::filesystem::path target(path);
    std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            MINIREDIS_LOG_ERROR("cluster") << "failed to create cluster config directory "
                                          << parent.string() << ": " << ec.message();
            return false;
        }
    }

    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            MINIREDIS_LOG_ERROR("cluster") << "failed to open cluster config temp file: " << tmp_path;
            return false;
        }
        if (!writeSnapshot(out, slot_map.ExportSnapshot())) {
            MINIREDIS_LOG_ERROR("cluster") << "failed to write cluster config: " << tmp_path;
            return false;
        }
        out.flush();
        if (!out) {
            MINIREDIS_LOG_ERROR("cluster") << "failed to flush cluster config: " << tmp_path;
            return false;
        }
    }

    if (!fsyncPath(tmp_path, false)) {
        MINIREDIS_LOG_ERROR("cluster") << "failed to fsync cluster config temp file: "
                                       << std::strerror(errno);
        std::filesystem::remove(tmp_path);
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        MINIREDIS_LOG_ERROR("cluster") << "failed to replace cluster config "
                                      << path << ": " << ec.message();
        std::filesystem::remove(tmp_path);
        return false;
    }
    const std::string parent_path = parent.empty() ? "." : parent.string();
    if (!fsyncPath(parent_path, true)) {
        MINIREDIS_LOG_WARN("cluster") << "failed to fsync cluster config directory: "
                                      << parent_path;
    }
    return true;
}

bool loadClusterConfig(const std::string& path, ClusterSlotMap& slot_map) {
    if (path.empty()) return false;

    std::ifstream input(path, std::ios::binary);
    if (!input) return false;

    std::string magic;
    std::getline(input, magic);
    if (magic != kClusterConfigMagic) {
        MINIREDIS_LOG_ERROR("cluster") << "invalid cluster config magic: " << path;
        return false;
    }

    ClusterSlotMapSnapshot snapshot;
    snapshot.slot_owner.assign(kRedisClusterSlots, "");
    snapshot.slot_meta.assign(kRedisClusterSlots, ClusterSlotMeta{});

    std::string tag;
    size_t node_count = 0;
    if (!(input >> tag >> snapshot.epoch) || tag != "epoch") return false;
    if (!(input >> tag >> node_count) || tag != "nodes") return false;
    if (node_count == 0 || node_count > kMaxClusterNodes) return false;

    std::unordered_set<std::string> nodes;
    for (size_t i = 0; i < node_count; ++i) {
        std::string node_tag;
        std::string node;
        std::string state_text;
        if (!(input >> node_tag >> node >> state_text) || node_tag != "node") return false;
        ClusterNodeState state = ClusterNodeState::Healthy;
        if (!parseClusterNodeState(state_text, state)) return false;
        if (node.empty() || !nodes.insert(node).second) return false;
        snapshot.nodes.push_back(node);
        snapshot.node_states[node] = state;
    }

    size_t range_count = 0;
    if (!(input >> tag >> range_count) || tag != "slots") return false;
    if (range_count > kRedisClusterSlots) return false;
    for (size_t i = 0; i < range_count; ++i) {
        std::string slot_tag;
        int start = 0;
        int end = 0;
        std::string owner;
        if (!(input >> slot_tag >> start >> end >> owner) || slot_tag != "slot") return false;
        if (start < 0 || end < start || end >= kRedisClusterSlots) return false;
        if (nodes.find(owner) == nodes.end()) return false;
        for (int slot = start; slot <= end; ++slot) {
            std::string& assigned_owner = snapshot.slot_owner[static_cast<size_t>(slot)];
            if (!assigned_owner.empty()) return false;
            assigned_owner = owner;
        }
    }

    size_t slot_state_count = 0;
    if (input >> tag) {
        if (tag != "slotstates") return false;
        if (!(input >> slot_state_count)) return false;
        if (slot_state_count > kRedisClusterSlots) return false;
        std::unordered_set<int> state_slots;
        for (size_t i = 0; i < slot_state_count; ++i) {
            std::string slot_state_tag;
            int slot = 0;
            std::string state_text;
            std::string peer;
            if (!(input >> slot_state_tag >> slot >> state_text >> peer) ||
                slot_state_tag != "slotstate") {
                return false;
            }
            if (slot < 0 || slot >= kRedisClusterSlots) return false;
            if (!state_slots.insert(slot).second) return false;
            if (nodes.find(peer) == nodes.end()) return false;
            ClusterSlotState state = ClusterSlotState::Stable;
            if (!parseClusterSlotState(state_text, state) || state == ClusterSlotState::Stable) {
                return false;
            }
            snapshot.slot_meta[static_cast<size_t>(slot)] = ClusterSlotMeta{state, peer};
        }
    }

    std::string trailing;
    if (input >> trailing) return false;

    if (!slot_map.LoadSnapshot(snapshot)) {
        MINIREDIS_LOG_ERROR("cluster") << "invalid cluster config content: " << path;
        return false;
    }

    MINIREDIS_LOG_INFO("cluster") << "loaded cluster config: " << path;
    return true;
}

} // namespace miniredis
