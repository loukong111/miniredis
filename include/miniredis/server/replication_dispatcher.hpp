#pragma once

#include "miniredis/server/replication_backlog.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace miniredis {

struct ReplicaDispatchStatus {
    std::string node;
    bool connected = false;
    uint64_t acknowledged_offset = 0;
    uint64_t target_offset = 0;
    uint64_t reconnects = 0;
    uint64_t errors = 0;
    uint64_t backlog_misses = 0;
    std::string last_error;
};

// Keeps one background connection per replica. Writers only append to the
// shared backlog and publish the newest offset; network latency stays off the
// client request path.
class ReplicationDispatcher {
public:
    ReplicationDispatcher(ReplicationBacklog& backlog,
                          std::string master_replid,
                          std::vector<std::string> replicas,
                          std::string password,
                          std::chrono::milliseconds reconnect_delay =
                              std::chrono::milliseconds(500));
    ~ReplicationDispatcher();

    ReplicationDispatcher(const ReplicationDispatcher&) = delete;
    ReplicationDispatcher& operator=(const ReplicationDispatcher&) = delete;

    void start();
    void stop();
    bool setMasterReplicationId(std::string replid);
    void notify(uint64_t offset);
    std::vector<ReplicaDispatchStatus> status() const;

private:
    struct Worker;

    void workerLoop(Worker& worker);
    bool connectReplica(Worker& worker);
    bool queryReplicaOffset(Worker& worker, uint64_t& offset, bool& replid_mismatch);
    bool sendEntry(Worker& worker, const ReplicationBacklogEntry& entry);
    void disconnect(Worker& worker);
    void recordFailure(Worker& worker, const std::string& error, bool backlog_miss = false);
    void publishStats() const;

    ReplicationBacklog& backlog_;
    std::string master_replid_;
    std::string password_;
    std::chrono::milliseconds reconnect_delay_;
    std::atomic<bool> running_{false};
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace miniredis
