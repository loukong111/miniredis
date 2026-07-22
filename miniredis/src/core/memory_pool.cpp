#include "miniredis/core/memory_pool.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>

namespace miniredis {

FixedMemoryPool::FixedMemoryPool(size_t num_blocks)
    : free_list_(nullptr), used_(0), free_(0) {
    std::lock_guard<std::mutex> lock(mutex_);
    grow_unlocked(num_blocks);
}

FixedMemoryPool::~FixedMemoryPool() {
    for (char* chunk : chunks_) {
        delete[] chunk;
    }
}

void FixedMemoryPool::grow_unlocked(size_t num_blocks) {
    if (num_blocks == 0) return;
    if (num_blocks > std::numeric_limits<size_t>::max() / BLOCK_SIZE) {
        throw std::bad_alloc();
    }
    // 分配一大块内存: num_blocks * BLOCK_SIZE
    auto owner = std::make_unique<char[]>(num_blocks * BLOCK_SIZE);
    char* chunk = owner.get();
    chunks_.push_back(chunk);
    owner.release();
    
    // 将这块内存切分成Block，串成链表
    Block* prev = nullptr;
    for (size_t i = 0; i < num_blocks; ++i) {
        Block* b = reinterpret_cast<Block*>(chunk + i * BLOCK_SIZE);
        b->next = prev;
        prev = b;
    }
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
        // 空闲不足，再扩展一批
        try {
            grow_unlocked(64);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }
    if (free_list_ == nullptr) return nullptr;
    Block* block = free_list_;
    free_list_ = block->next;
    used_.fetch_add(1, std::memory_order_relaxed);
    free_.fetch_sub(1, std::memory_order_relaxed);
    // 这里直接返回Block起始地址，因为用户使用大小不会超过BLOCK_SIZE
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
