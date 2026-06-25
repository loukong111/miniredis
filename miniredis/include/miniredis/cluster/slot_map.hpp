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

class ClusterSlotMap {
public:
    ClusterSlotMap();

    void Rebuild(const std::vector<std::string>& nodes);
    void clear();
    std::string GetNodeForSlot(uint16_t slot) const;
    std::vector<std::string> GetAllNodes() const;
    std::vector<SlotRange> GetSlotRangesForNode(const std::string& node) const;
    uint64_t GetEpoch() const;
    size_t AssignedSlotCount() const;
    void MarkNodeHealthy(const std::string& node);
    void MarkNodeFailed(const std::string& node);
    bool IsNodeFailed(const std::string& node) const;
    size_t FailedNodeCount() const;

private:
    std::array<std::string, kRedisClusterSlots> slot_owner_;
    std::vector<std::string> nodes_;
    std::unordered_map<std::string, bool> node_failed_;
    uint64_t epoch_ = 0;
    mutable std::mutex mutex_;
};

} 
