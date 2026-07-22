#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

namespace miniredis {

class FixedMemoryPool {
public:
    static constexpr size_t BLOCK_SIZE = 64;
    explicit FixedMemoryPool(size_t num_blocks = 1024);
    ~FixedMemoryPool();

    FixedMemoryPool(const FixedMemoryPool&) = delete;
    FixedMemoryPool& operator=(const FixedMemoryPool&) = delete;

    void* allocate();
    void deallocate(void* ptr);

    size_t used_blocks() const { return used_.load(std::memory_order_relaxed); }
    size_t free_blocks() const { return free_.load(std::memory_order_relaxed); }

private:
    struct Block { Block* next; };
    Block* free_list_;
    std::mutex mutex_;
    std::vector<char*> chunks_;
    std::atomic<size_t> used_;
    std::atomic<size_t> free_;

    void grow_unlocked(size_t num_blocks);
};

}
