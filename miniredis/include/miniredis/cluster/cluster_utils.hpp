#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace miniredis {

constexpr uint16_t kRedisClusterSlots = 16384;

std::string_view clusterHashKey(std::string_view key);
uint16_t clusterHashSlot(std::string_view key);
std::string clusterNodeId(const std::string& node_addr);

} // namespace miniredis
