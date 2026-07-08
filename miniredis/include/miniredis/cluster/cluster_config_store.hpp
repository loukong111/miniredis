#pragma once

#include "miniredis/cluster/slot_map.hpp"
#include <string>

namespace miniredis {

bool saveClusterConfig(const std::string& path, const ClusterSlotMap& slot_map);
bool loadClusterConfig(const std::string& path, ClusterSlotMap& slot_map);

} // namespace miniredis
