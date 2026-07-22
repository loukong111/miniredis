#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <array>
#include <cstdint>
#include <unordered_map>
#include "miniredis/cluster/cluster_utils.hpp"

namespace miniredis {

struct SlotRange {
    uint16_t start;
    uint16_t end;
};

enum class ClusterNodeState {
    Healthy,
    Suspect,
    Fail
};

enum class ClusterSlotState {
    Stable,
    Migrating,
    Importing
};

const char* clusterNodeStateName(ClusterNodeState state);
bool parseClusterNodeState(const std::string& value, ClusterNodeState& state);
const char* clusterSlotStateName(ClusterSlotState state);
bool parseClusterSlotState(const std::string& value, ClusterSlotState& state);

struct ClusterSlotMeta {
    ClusterSlotState state = ClusterSlotState::Stable;
    std::string peer_node;
};

struct ClusterSlotMapSnapshot {
    std::vector<std::string> nodes;
    std::vector<std::string> slot_owner;
    std::vector<ClusterSlotMeta> slot_meta;
    std::unordered_map<std::string, ClusterNodeState> node_states;
    uint64_t epoch = 0;
};

class ClusterSlotMap {
public:
    ClusterSlotMap();

    void Rebuild(const std::vector<std::string>& nodes);
    bool LoadSnapshot(const ClusterSlotMapSnapshot& snapshot);
    bool LoadSnapshotIfNewer(const ClusterSlotMapSnapshot& snapshot);
    ClusterSlotMapSnapshot ExportSnapshot() const;
    void clear();
    bool AddNode(const std::string& node);
    bool RemoveNode(const std::string& node);
    bool NodeOwnsSlots(const std::string& node) const;
    std::string GetNodeForSlot(uint16_t slot) const;
    bool SetSlotOwner(uint16_t slot, const std::string& node);
    bool SetSlotState(uint16_t slot, ClusterSlotState state, const std::string& peer_node = "");
    ClusterSlotMeta GetSlotMeta(uint16_t slot) const;
    std::vector<std::string> GetAllNodes() const;
    std::vector<SlotRange> GetSlotRangesForNode(const std::string& node) const;
    uint64_t GetEpoch() const;
    size_t AssignedSlotCount() const;
    void MarkNodeHealthy(const std::string& node);
    void MarkNodeSuspect(const std::string& node);
    void MarkNodeFailed(const std::string& node);
    void SetNodeState(const std::string& node, ClusterNodeState state);
    ClusterNodeState GetNodeState(const std::string& node) const;
    bool IsNodeFailed(const std::string& node) const;
    size_t FailedNodeCount() const;
    size_t SuspectNodeCount() const;

private:
    bool LoadSnapshotInternal(const ClusterSlotMapSnapshot& snapshot, bool only_if_newer);

    std::array<std::string, kRedisClusterSlots> slot_owner_;
    std::array<ClusterSlotMeta, kRedisClusterSlots> slot_meta_;
    std::vector<std::string> nodes_;
    std::unordered_map<std::string, ClusterNodeState> node_states_;
    uint64_t epoch_ = 0;
    mutable std::mutex mutex_;
};

} 
