#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace miniredis {

struct FailoverState {
    uint64_t current_epoch = 0;
    uint64_t last_voted_epoch = 0;
    std::string voted_for;
    uint64_t leader_epoch = 0;
    std::string leader;
};

bool loadFailoverStateFile(const std::string& path, FailoverState& state);
bool saveFailoverStateFile(const std::string& path, const FailoverState& state);

// Persists election terms and votes before exposing them to callers. This is
// the minimum safety boundary needed to avoid voting for two candidates in the
// same epoch after a process restart.
class FailoverCoordinator {
public:
    FailoverCoordinator(std::string node, std::string state_file);

    bool restore();
    FailoverState snapshot() const;
    uint64_t beginElection(uint64_t minimum_epoch = 0);
    bool grantVote(uint64_t epoch, const std::string& candidate,
                   uint64_t candidate_offset, uint64_t local_offset,
                   bool master_failed);
    bool recordLeader(uint64_t epoch, const std::string& leader);

private:
    bool commit(const FailoverState& next);

    std::string node_;
    std::string state_file_;
    mutable std::mutex mutex_;
    FailoverState state_;
};

} // namespace miniredis
