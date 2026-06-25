#include "miniredis/cluster/slot_map.hpp"
#include <unordered_set>

namespace miniredis {

ClusterSlotMap::ClusterSlotMap() {
    slot_owner_.fill("");
}

void ClusterSlotMap::Rebuild(const std::vector<std::string>& nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_owner_.fill("");
    nodes_.clear();

    std::unordered_set<std::string> seen;
    for (const auto& node : nodes) {
        if (!node.empty() && seen.insert(node).second) {
            nodes_.push_back(node);
            node_failed_.try_emplace(node, false);
        }
    }
    for (auto it = node_failed_.begin(); it != node_failed_.end(); ) {
        if (seen.find(it->first) == seen.end()) {
            it = node_failed_.erase(it);
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

void ClusterSlotMap::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_owner_.fill("");
    nodes_.clear();
    node_failed_.clear();
    ++epoch_;
}

std::string ClusterSlotMap::GetNodeForSlot(uint16_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= kRedisClusterSlots) return "";
    return slot_owner_[slot];
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
    std::lock_guard<std::mutex> lock(mutex_);
    node_failed_[node] = false;
}

void ClusterSlotMap::MarkNodeFailed(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_failed_[node] = true;
}

bool ClusterSlotMap::IsNodeFailed(const std::string& node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_failed_.find(node);
    return it != node_failed_.end() && it->second;
}

size_t ClusterSlotMap::FailedNodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [node, failed] : node_failed_) {
        if (failed) ++count;
    }
    return count;
}

}
