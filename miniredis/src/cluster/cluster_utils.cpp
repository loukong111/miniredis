#include "miniredis/cluster/cluster_utils.hpp"
#include <functional>
#include <iomanip>
#include <sstream>

namespace miniredis {
namespace {

uint16_t crc16(std::string_view data) {
    uint16_t crc = 0;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

} // namespace

std::string_view clusterHashKey(std::string_view key) {
    size_t open = key.find('{');
    if (open == std::string_view::npos) return key;

    size_t close = key.find('}', open + 1);
    if (close == std::string_view::npos || close == open + 1) return key;

    return key.substr(open + 1, close - open - 1);
}

uint16_t clusterHashSlot(std::string_view key) {
    return static_cast<uint16_t>(crc16(clusterHashKey(key)) % kRedisClusterSlots);
}

std::string clusterNodeId(const std::string& node_addr) {
    size_t value = std::hash<std::string>{}(node_addr);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 5; ++i) {
        oss << std::setw(8) << static_cast<uint32_t>(value >> ((i % 2) * 32));
    }
    return oss.str().substr(0, 40);
}

} // namespace miniredis
