#pragma once

#include <atomic>
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

    void setNodeAddr(const std::string& addr) { node_addr_ = addr; }
    std::string getNodeAddr() const { return node_addr_; }

    void setKeyCount(size_t count) { key_count_.store(count, std::memory_order_relaxed); }
    void setMemoryPoolUsed(size_t used_blocks, size_t free_blocks);

    std::string toJson() const;

private:
    Stats() = default;

    std::atomic<size_t> total_commands_{0};
    std::atomic<size_t> get_hits_{0};
    std::atomic<size_t> get_misses_{0};
    std::atomic<size_t> key_count_{0};
    std::atomic<size_t> mem_pool_used_{0};
    std::atomic<size_t> mem_pool_free_{0};
    std::string node_addr_;
};

}