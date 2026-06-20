#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <array>
#include <cstdint>
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

private:
    std::array<std::string, kRedisClusterSlots> slot_owner_;
    std::vector<std::string> nodes_;
    mutable std::mutex mutex_;
};

} 
