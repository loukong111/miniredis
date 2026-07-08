#include "miniredis/cluster/slot_map.hpp"
#include <algorithm>
#include <unordered_set>

namespace miniredis {

const char* clusterNodeStateName(ClusterNodeState state) {
    switch (state) {
        case ClusterNodeState::Healthy: return "healthy";
        case ClusterNodeState::Suspect: return "suspect";
        case ClusterNodeState::Fail: return "fail";
    }
    return "healthy";
}

bool parseClusterNodeState(const std::string& value, ClusterNodeState& state) {
    if (value == "healthy") {
        state = ClusterNodeState::Healthy;
        return true;
    }
    if (value == "suspect") {
        state = ClusterNodeState::Suspect;
        return true;
    }
    if (value == "fail") {
        state = ClusterNodeState::Fail;
        return true;
    }
    return false;
}

const char* clusterSlotStateName(ClusterSlotState state) {
    switch (state) {
        case ClusterSlotState::Stable: return "stable";
        case ClusterSlotState::Migrating: return "migrating";
        case ClusterSlotState::Importing: return "importing";
    }
    return "stable";
}

bool parseClusterSlotState(const std::string& value, ClusterSlotState& state) {
    if (value == "stable") {
        state = ClusterSlotState::Stable;
        return true;
    }
    if (value == "migrating") {
        state = ClusterSlotState::Migrating;
        return true;
    }
    if (value == "importing") {
        state = ClusterSlotState::Importing;
        return true;
    }
    return false;
}

ClusterSlotMap::ClusterSlotMap() {
    slot_owner_.fill("");
}

void ClusterSlotMap::Rebuild(const std::vector<std::string>& nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_owner_.fill("");
    slot_meta_.fill(ClusterSlotMeta{});
    nodes_.clear();

    std::unordered_set<std::string> seen;
    for (const auto& node : nodes) {
        if (!node.empty() && seen.insert(node).second) {
            nodes_.push_back(node);
            node_states_.try_emplace(node, ClusterNodeState::Healthy);
        }
    }
    for (auto it = node_states_.begin(); it != node_states_.end(); ) {
        if (seen.find(it->first) == seen.end()) {
            it = node_states_.erase(it);
        } else {
            ++it;
        }
    }
    if (nodes_.empty()) return;

    const size_t node_count = nodes_.size();
    for (size_t i = 0; i < kRedisClusterSlots; ++i) {
        size_t node_index = (i * node_count) / kRedisClusterSlots;
        slot_owner_[i] = nodes_[node_index];
    }
    ++epoch_;
}

bool ClusterSlotMap::LoadSnapshot(const ClusterSlotMapSnapshot& snapshot) {
    if (snapshot.slot_owner.size() != kRedisClusterSlots) return false;
    if (!snapshot.slot_meta.empty() && snapshot.slot_meta.size() != kRedisClusterSlots) return false;

    std::unordered_set<std::string> seen;
    std::vector<std::string> nodes;
    nodes.reserve(snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        if (!node.empty() && seen.insert(node).second) {
            nodes.push_back(node);
        }
    }
    if (nodes.empty()) return false;

    for (const auto& owner : snapshot.slot_owner) {
        if (!owner.empty() && seen.find(owner) == seen.end()) {
            return false;
        }
    }
    for (const auto& meta : snapshot.slot_meta) {
        if (!meta.peer_node.empty() && seen.find(meta.peer_node) == seen.end()) {
            return false;
        }
        if (meta.state != ClusterSlotState::Stable && meta.peer_node.empty()) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    nodes_ = std::move(nodes);
    std::copy(snapshot.slot_owner.begin(), snapshot.slot_owner.end(), slot_owner_.begin());
    slot_meta_.fill(ClusterSlotMeta{});
    if (!snapshot.slot_meta.empty()) {
        std::copy(snapshot.slot_meta.begin(), snapshot.slot_meta.end(), slot_meta_.begin());
    }
    node_states_.clear();
    for (const auto& node : nodes_) {
        auto it = snapshot.node_states.find(node);
        node_states_[node] = (it == snapshot.node_states.end())
                                 ? ClusterNodeState::Healthy
                                 : it->second;
    }
    epoch_ = snapshot.epoch;
    return true;
}

bool ClusterSlotMap::LoadSnapshotIfNewer(const ClusterSlotMapSnapshot& snapshot) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot.epoch <= epoch_) return false;
    }
    return LoadSnapshot(snapshot);
}

ClusterSlotMapSnapshot ClusterSlotMap::ExportSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ClusterSlotMapSnapshot snapshot;
    snapshot.nodes = nodes_;
    snapshot.slot_owner.assign(slot_owner_.begin(), slot_owner_.end());
    snapshot.slot_meta.assign(slot_meta_.begin(), slot_meta_.end());
    snapshot.node_states = node_states_;
    snapshot.epoch = epoch_;
    return snapshot;
}

void ClusterSlotMap::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_owner_.fill("");
    slot_meta_.fill(ClusterSlotMeta{});
    nodes_.clear();
    node_states_.clear();
    ++epoch_;
}

std::string ClusterSlotMap::GetNodeForSlot(uint16_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kRedisClusterSlots) return "";
    return slot_owner_[slot];
}

bool ClusterSlotMap::SetSlotOwner(uint16_t slot, const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kRedisClusterSlots || node.empty()) return false;
    if (std::find(nodes_.begin(), nodes_.end(), node) == nodes_.end()) {
        return false;
    }
    if (slot_owner_[slot] == node && slot_meta_[slot].state == ClusterSlotState::Stable) {
        return true;
    }
    slot_owner_[slot] = node;
    slot_meta_[slot] = ClusterSlotMeta{};
    ++epoch_;
    return true;
}

bool ClusterSlotMap::SetSlotState(uint16_t slot, ClusterSlotState state, const std::string& peer_node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kRedisClusterSlots) return false;
    if (state != ClusterSlotState::Stable) {
        if (peer_node.empty() || std::find(nodes_.begin(), nodes_.end(), peer_node) == nodes_.end()) {
            return false;
        }
    }
    ClusterSlotMeta next;
    next.state = state;
    next.peer_node = (state == ClusterSlotState::Stable) ? "" : peer_node;
    if (slot_meta_[slot].state == next.state && slot_meta_[slot].peer_node == next.peer_node) {
        return true;
    }
    slot_meta_[slot] = std::move(next);
    ++epoch_;
    return true;
}

ClusterSlotMeta ClusterSlotMap::GetSlotMeta(uint16_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kRedisClusterSlots) return {};
    return slot_meta_[slot];
}

std::vector<std::string> ClusterSlotMap::GetAllNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_;
}

std::vector<SlotRange> ClusterSlotMap::GetSlotRangesForNode(const std::string& node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SlotRange> ranges;
    bool in_range = false;
    uint16_t start = 0;

    for (uint16_t slot = 0; slot < kRedisClusterSlots; ++slot) {
        bool owns = (slot_owner_[slot] == node);
        if (owns && !in_range) {
            start = slot;
            in_range = true;
        } else if (!owns && in_range) {
            ranges.push_back({start, static_cast<uint16_t>(slot - 1)});
            in_range = false;
        }
    }
    if (in_range) {
        ranges.push_back({start, static_cast<uint16_t>(kRedisClusterSlots - 1)});
    }
    return ranges;
}

uint64_t ClusterSlotMap::GetEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

size_t ClusterSlotMap::AssignedSlotCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& owner : slot_owner_) {
        if (!owner.empty()) ++count;
    }
    return count;
}

void ClusterSlotMap::MarkNodeHealthy(const std::string& node) {
    SetNodeState(node, ClusterNodeState::Healthy);
}

void ClusterSlotMap::MarkNodeSuspect(const std::string& node) {
    SetNodeState(node, ClusterNodeState::Suspect);
}

void ClusterSlotMap::MarkNodeFailed(const std::string& node) {
    SetNodeState(node, ClusterNodeState::Fail);
}

void ClusterSlotMap::SetNodeState(const std::string& node, ClusterNodeState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node.empty()) return;
    bool changed = false;
    if (std::find(nodes_.begin(), nodes_.end(), node) == nodes_.end()) {
        nodes_.push_back(node);
        changed = true;
    }
    auto it = node_states_.find(node);
    if (it == node_states_.end()) {
        node_states_[node] = state;
        changed = true;
    } else if (it->second != state) {
        it->second = state;
        changed = true;
    }
    if (changed) ++epoch_;
}

ClusterNodeState ClusterSlotMap::GetNodeState(const std::string& node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_states_.find(node);
    return it == node_states_.end() ? ClusterNodeState::Healthy : it->second;
}

bool ClusterSlotMap::IsNodeFailed(const std::string& node) const {
    return GetNodeState(node) == ClusterNodeState::Fail;
}

size_t ClusterSlotMap::FailedNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [node, state] : node_states_) {
        if (state == ClusterNodeState::Fail) ++count;
    }
    return count;
}

size_t ClusterSlotMap::SuspectNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [node, state] : node_states_) {
        if (state == ClusterNodeState::Suspect) ++count;
    }
    return count;
}

}
