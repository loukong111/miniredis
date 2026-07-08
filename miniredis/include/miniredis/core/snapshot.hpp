#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace miniredis {

struct SnapshotEntry {
    std::string value;
    uint64_t expire_at_ms = 0; // Unix time in milliseconds; 0 means no TTL.

    bool operator==(const SnapshotEntry& other) const = default;
};

using SnapshotData = std::unordered_map<std::string, SnapshotEntry>;

} // namespace miniredis
