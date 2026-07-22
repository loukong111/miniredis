#include "miniredis/server/failover_coordinator.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace miniredis {
namespace {

constexpr std::string_view kStateMagic = "MINIREDIS_FAILOVER_STATE_V1";
constexpr size_t kMaxStateFileBytes = 4096;

bool validNode(const std::string& node) {
    return !node.empty() && node.size() <= 255 &&
           node.find_first_of(" \t\r\n") == std::string::npos;
}

bool writeAll(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool fsyncParent(const std::filesystem::path& path) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                   : path.parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    return ok && closed;
}

} // namespace

bool loadFailoverStateFile(const std::string& path, FailoverState& state) {
    state = FailoverState{};
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxStateFileBytes) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    std::istringstream input(contents);

    std::string magic;
    std::string epoch_key;
    std::string vote_epoch_key;
    std::string voted_for_key;
    std::string leader_epoch_key;
    std::string leader_key;
    std::string voted_for;
    std::string leader;
    std::string extra;
    if (!(input >> magic >> epoch_key >> state.current_epoch >>
          vote_epoch_key >> state.last_voted_epoch >> voted_for_key >> voted_for >>
          leader_epoch_key >> state.leader_epoch >> leader_key >> leader) ||
        (input >> extra) || magic != kStateMagic || epoch_key != "current_epoch" ||
        vote_epoch_key != "last_voted_epoch" || voted_for_key != "voted_for" ||
        leader_epoch_key != "leader_epoch" || leader_key != "leader") {
        state = FailoverState{};
        return false;
    }

    state.voted_for = voted_for == "-" ? "" : std::move(voted_for);
    state.leader = leader == "-" ? "" : std::move(leader);
    if ((!state.voted_for.empty() && !validNode(state.voted_for)) ||
        (!state.leader.empty() && !validNode(state.leader)) ||
        state.last_voted_epoch > state.current_epoch ||
        state.leader_epoch > state.current_epoch ||
        (state.last_voted_epoch == 0) != state.voted_for.empty() ||
        (state.leader_epoch == 0) != state.leader.empty()) {
        state = FailoverState{};
        return false;
    }
    return true;
}

bool saveFailoverStateFile(const std::string& path, const FailoverState& state) {
    if (path.empty() || state.last_voted_epoch > state.current_epoch ||
        state.leader_epoch > state.current_epoch ||
        (!state.voted_for.empty() && !validNode(state.voted_for)) ||
        (!state.leader.empty() && !validNode(state.leader)) ||
        (state.last_voted_epoch == 0) != state.voted_for.empty() ||
        (state.leader_epoch == 0) != state.leader.empty()) {
        return false;
    }

    const std::filesystem::path destination(path);
    if (!destination.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return false;
    }

    std::ostringstream output;
    output << kStateMagic << '\n'
           << "current_epoch " << state.current_epoch << '\n'
           << "last_voted_epoch " << state.last_voted_epoch << '\n'
           << "voted_for " << (state.voted_for.empty() ? "-" : state.voted_for) << '\n'
           << "leader_epoch " << state.leader_epoch << '\n'
           << "leader " << (state.leader.empty() ? "-" : state.leader) << '\n';

    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string contents = output.str();
    bool ok = writeAll(fd, contents) && ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return fsyncParent(destination);
}

FailoverCoordinator::FailoverCoordinator(std::string node, std::string state_file)
    : node_(std::move(node)), state_file_(std::move(state_file)) {}

bool FailoverCoordinator::restore() {
    std::lock_guard<std::mutex> lock(mutex_);
    FailoverState loaded;
    if (!loadFailoverStateFile(state_file_, loaded)) {
        std::error_code ec;
        if (std::filesystem::exists(state_file_, ec)) return false;
        state_ = FailoverState{};
        return true;
    }
    state_ = std::move(loaded);
    return true;
}

FailoverState FailoverCoordinator::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool FailoverCoordinator::commit(const FailoverState& next) {
    if (!saveFailoverStateFile(state_file_, next)) return false;
    state_ = next;
    return true;
}

uint64_t FailoverCoordinator::beginElection(uint64_t minimum_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t base = std::max(state_.current_epoch, minimum_epoch);
    if (base == std::numeric_limits<uint64_t>::max()) return 0;
    FailoverState next = state_;
    next.current_epoch = base + 1;
    next.last_voted_epoch = next.current_epoch;
    next.voted_for = node_;
    next.leader_epoch = 0;
    next.leader.clear();
    return commit(next) ? next.current_epoch : 0;
}

bool FailoverCoordinator::grantVote(uint64_t epoch, const std::string& candidate,
                                    uint64_t candidate_offset, uint64_t local_offset,
                                    bool master_failed) {
    if (epoch == 0 || !validNode(candidate) || !master_failed ||
        candidate_offset < local_offset) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch < state_.current_epoch ||
        (state_.last_voted_epoch == epoch && state_.voted_for != candidate)) {
        return false;
    }
    if (state_.last_voted_epoch == epoch && state_.voted_for == candidate) return true;

    FailoverState next = state_;
    next.current_epoch = epoch;
    next.last_voted_epoch = epoch;
    next.voted_for = candidate;
    if (next.leader_epoch < epoch) {
        next.leader_epoch = 0;
        next.leader.clear();
    }
    return commit(next);
}

bool FailoverCoordinator::recordLeader(uint64_t epoch, const std::string& leader) {
    if (epoch == 0 || !validNode(leader)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch < state_.current_epoch || epoch < state_.leader_epoch) return false;
    if (epoch == state_.leader_epoch && !state_.leader.empty() && state_.leader != leader) {
        return false;
    }

    FailoverState next = state_;
    next.current_epoch = std::max(next.current_epoch, epoch);
    next.leader_epoch = epoch;
    next.leader = leader;
    return commit(next);
}

} // namespace miniredis
