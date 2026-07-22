#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace miniredis {

inline constexpr size_t kReplicationIdLength = 40;

struct ReplicationState {
    std::string master_replid;
    uint64_t offset = 0;
};

bool isValidReplicationId(std::string_view replid);
std::string generateReplicationId();

// The loader also accepts the legacy single-offset format. Legacy state has an
// empty replid and therefore always requires one safe full resynchronization.
bool loadReplicationStateFile(const std::string& path, ReplicationState& state);
bool saveReplicationStateFile(const std::string& path, const ReplicationState& state);

} // namespace miniredis
