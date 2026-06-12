#pragma once

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <optional>
#include <memory>
#include "miniredis/core/memory_pool.hpp"   // 添加内存池头文件

namespace miniredis {

// 缓存条目，支持 TTL，value 使用内存池存储
struct CacheEntry {
    char* data = nullptr;          // 数据指针（可能来自内存池或 new[]）
    size_t size = 0;               // 实际数据长度
    bool is_pool_allocated = true; // 标记是否由内存池分配
    std::chrono::steady_clock::time_point expire_time;

    bool is_expired() const {
        return expire_time != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() > expire_time;
    }

    bool setValue(const std::string& value, FixedMemoryPool& pool);
    std::string getValue() const;
    void release(FixedMemoryPool& pool);
};

class CacheStore {
public:
    explicit CacheStore(FixedMemoryPool& pool);  // 依赖外部内存池
    ~CacheStore();

    size_t key_count() const;

    void set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
    size_t cleanup();   // 清理过期条目，释放内存池内存

    std::unordered_map<std::string, std::string> snapshot() const;
    void load_snapshot(const std::unordered_map<std::string, std::string>& data);

private:
    FixedMemoryPool& pool_;
    mutable std::shared_mutex mtx_;
    std::unordered_map<std::string, CacheEntry> store_;
};

} // namespace miniredis