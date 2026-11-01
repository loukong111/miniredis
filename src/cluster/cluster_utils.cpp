#include "miniredis/cluster/cluster_utils.hpp"
#include <charconv>
#include <functional>
#include <iomanip>
#include <sstream>

namespace miniredis {
namespace {

uint16_t crc16(std::string_view data) {
    uint16_t crc = 0;
    for (char raw_byte : data) {
        const auto byte = static_cast<unsigned char>(raw_byte);
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

bool parseNodeAddress(const std::string& node, std::string& host, int& port) {
    const size_t separator = node.rfind(':');
    if (separator == std::string::npos || separator == 0) return false;
    const char* begin = node.data() + separator + 1;
    const char* end = node.data() + node.size();
    int parsed_port = 0;
    const auto [ptr, ec] = std::from_chars(begin, end, parsed_port);
    if (ec != std::errc() || ptr != end || parsed_port < 1 || parsed_port > 65535) {
        return false;
    }
    host = node.substr(0, separator);
    port = parsed_port;
    return true;
}

} // namespace miniredis
