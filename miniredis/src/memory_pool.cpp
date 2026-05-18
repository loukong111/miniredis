#include "memory_pool.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

namespace miniredis {

FixedMemoryPool::FixedMemoryPool(size_t num_blocks)
    : free_list_(nullptr), used_(0), free_(0) {
    grow(num_blocks);
}

FixedMemoryPool::~FixedMemoryPool() {
    for (char* chunk : chunks_) {
        delete[] chunk;
    }
}

void FixedMemoryPool::grow(size_t num_blocks) {
    // 分配一大块内存: num_blocks * BLOCK_SIZE
    char* chunk = new char[num_blocks * BLOCK_SIZE];
    chunks_.push_back(chunk);
    
    // 将这块内存切分成Block，串成链表
    Block* prev = nullptr;
    for (size_t i = 0; i < num_blocks; ++i) {
        Block* b = reinterpret_cast<Block*>(chunk + i * BLOCK_SIZE);
        b->next = prev;
        prev = b;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // 新空闲块链到现有free_list_头部
    Block* old_head = free_list_;
    free_list_ = prev;
    // 需要遍历新链表的尾部接到old_head
    Block* tail = prev;
    while (tail && tail->next) {
        tail = tail->next;
    }
    if (tail) tail->next = old_head;
    free_.fetch_add(num_blocks, std::memory_order_relaxed);
}

void* FixedMemoryPool::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_ == nullptr) {
        // 空闲不足，再扩展一批 (比如64块)
        grow(64);
    }
    Block* block = free_list_;
    free_list_ = block->next;
    used_.fetch_add(1, std::memory_order_relaxed);
    free_.fetch_sub(1, std::memory_order_relaxed);
    // 返回指针，注意这里直接返回Block起始地址，用户使用大小不超过BLOCK_SIZE
    return reinterpret_cast<void*>(block);
}

void FixedMemoryPool::deallocate(void* ptr) {
    if (!ptr) return;
    Block* block = reinterpret_cast<Block*>(ptr);
    std::lock_guard<std::mutex> lock(mutex_);
    block->next = free_list_;
    free_list_ = block;
    used_.fetch_sub(1, std::memory_order_relaxed);
    free_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace miniredis