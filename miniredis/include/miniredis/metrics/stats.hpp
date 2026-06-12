#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <cstddef>

namespace miniredis {

class Stats {
public:
    static Stats& instance();

    void recordCommand(const std::string& cmd, bool hit = false);
    void recordSet();
    void recordGetHit();
    void recordGetMiss();
    void recordConnectionOpen();
    void recordConnectionClose();
    void recordRejectedConnection();
    void recordCommandLatency(size_t latency_us);

    void setNodeAddr(const std::string& addr);
    std::string getNodeAddr() const;

    void setKeyCount(size_t count) { key_count_.store(count, std::memory_order_relaxed); }
    void setMemoryPoolUsed(size_t used_blocks, size_t free_blocks);

    std::string toJson() const;

private:
    Stats() = default;//单例，私有化

    std::atomic<size_t> total_commands_{0};
    std::atomic<size_t> get_hits_{0};
    std::atomic<size_t> get_misses_{0};
    std::atomic<size_t> key_count_{0};
    std::atomic<size_t> mem_pool_used_{0};
    std::atomic<size_t> mem_pool_free_{0};
    std::atomic<size_t> connected_clients_{0};
    std::atomic<size_t> total_connections_{0};
    std::atomic<size_t> rejected_connections_{0};
    std::atomic<size_t> latency_samples_{0};
    std::atomic<size_t> total_command_latency_us_{0};
    std::atomic<size_t> max_command_latency_us_{0};
    std::string node_addr_;
    mutable std::mutex node_mutex_;
};

}
