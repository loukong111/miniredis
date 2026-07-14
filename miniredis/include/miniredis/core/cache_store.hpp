#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <optional>
#include <memory>
#include <vector>
#include "miniredis/core/memory_pool.hpp"   // 添加内存池头文件
#include "miniredis/core/snapshot.hpp"

namespace miniredis {

enum class EvictionPolicy {
    NoEviction,
    Lru
};

enum class SetResult {
    Ok,
    OutOfMemory,
    AllocationFailed
};

enum class IncrementResultCode {
    Ok,
    NotInteger,
    Overflow,
    OutOfMemory,
    AllocationFailed
};

struct ValueUpdateResult {
    SetResult status = SetResult::Ok;
    bool changed = false;
    std::string value;
    size_t length = 0;
    int ttl_seconds = 0;
};

struct IncrementResult {
    IncrementResultCode code = IncrementResultCode::Ok;
    long long number = 0;
    std::string value;
    int ttl_seconds = 0;
};

// 缓存条目，支持 TTL，value 使用内存池存储
struct CacheEntry {
    char* data = nullptr;          // 数据指针（可能来自内存池或 new[]）
    size_t size = 0;               // 实际数据长度
    bool is_pool_allocated = true; // 标记是否由内存池分配
    std::chrono::steady_clock::time_point expire_time;
    std::chrono::steady_clock::time_point last_access_time;

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
    explicit CacheStore(FixedMemoryPool& pool, size_t shard_count = 1);  // 依赖外部内存池
    ~CacheStore();

    size_t shard_count() const { return shards_.size(); }
    size_t key_count() const;
    size_t used_memory_bytes() const;
    size_t max_memory_bytes() const;
    size_t evicted_keys() const;
    void configure_memory_limit(size_t max_memory_bytes, EvictionPolicy policy);

    SetResult set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    ValueUpdateResult set_if_absent(const std::string& key, const std::string& value);
    ValueUpdateResult append(const std::string& key, const std::string& suffix);
    IncrementResult increment(const std::string& key, long long delta);
    std::optional<std::string> get(const std::string& key);
    std::optional<std::string> get_and_delete(const std::string& key);
    std::optional<std::string> get_and_expire(const std::string& key, int64_t ttl_ms);
    bool persist(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
    bool expire(const std::string& key, int ttl_seconds);
    bool pexpire(const std::string& key, int64_t ttl_ms);
    long long ttl(const std::string& key);
    long long pttl(const std::string& key);
    size_t cleanup();   // 清理过期条目，释放内存池内存
    std::vector<std::string> keys() const;

    SnapshotData snapshot() const;
    void load_snapshot(const SnapshotData& data);

private:
    struct Shard {
        mutable std::shared_mutex mtx;
        std::unordered_map<std::string, CacheEntry> store;
        size_t used_memory_bytes = 0;
        size_t evicted_keys = 0;
    };

    Shard& shard_for(const std::string& key);
    const Shard& shard_for(const std::string& key) const;
    size_t entry_memory(const std::string& key, const CacheEntry& entry) const;
    void erase_entry(Shard& shard, std::unordered_map<std::string, CacheEntry>::iterator it);
    size_t cleanup_expired_locked(Shard& shard);
    bool evict_lru_until_fits(Shard& shard, const std::string& protected_key,
                              size_t required_bytes);
    size_t shard_memory_limit() const;

    FixedMemoryPool& pool_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<size_t> max_memory_bytes_{0};
    std::atomic<EvictionPolicy> eviction_policy_{EvictionPolicy::NoEviction};
};

} // namespace miniredis
