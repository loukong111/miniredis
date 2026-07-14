#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/logger.hpp"
#include <charconv>
#include <limits>
#include <mutex>
#include <cstring>
#include <iostream>
#include <new>

namespace miniredis {
namespace {

constexpr uint64_t kMaxSnapshotTtlMillis = 100ULL * 365ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

uint64_t currentUnixMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool parseInt64Strict(const std::string& value, long long& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size();
}

bool addWouldOverflow(long long lhs, long long rhs) {
    if (rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) return true;
    if (rhs == std::numeric_limits<long long>::min()) return lhs < 0;
    if (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs) return true;
    return false;
}

int ttlSecondsForAof(const CacheEntry& entry) {
    if (entry.expire_time == std::chrono::steady_clock::time_point{}) return 0;
    auto remaining = entry.expire_time - std::chrono::steady_clock::now();
    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (remaining_ms <= 0) return 0;
    auto seconds = (remaining_ms + 999) / 1000;
    if (seconds > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(seconds);
}

} // namespace

bool CacheEntry::setValue(const std::string& value, FixedMemoryPool& pool) {
    // 释放原有内存
    release(pool);
    
    size = value.size();
    // 判断是否使用内存池（<= BLOCK_SIZE）
    if (size <= FixedMemoryPool::BLOCK_SIZE) {
        data = static_cast<char*>(pool.allocate());
        if (!data) return false;
        is_pool_allocated = true;
        std::memcpy(data, value.data(), size);
    } else {
        // 大值：直接 new 字节数组
        data = new (std::nothrow) char[size];
        if (!data) return false;
        is_pool_allocated = false;
        std::memcpy(data, value.data(), size);
    }
    return true;
}

std::string CacheEntry::getValue() const {
    if (data == nullptr) return "";
    return std::string(data, size);
}

void CacheEntry::release(FixedMemoryPool& pool) {
    if (data == nullptr) return;
    if (is_pool_allocated) {
        pool.deallocate(data);
    } else {
        delete[] data;
    }
    data = nullptr;
    size = 0;
    is_pool_allocated = true; 
}

CacheStore::CacheStore(FixedMemoryPool& pool, size_t shard_count) : pool_(pool) {
    if (shard_count == 0) shard_count = 1;
    shards_.reserve(shard_count);
    for (size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

CacheStore::~CacheStore() {
    for (auto& shard : shards_) {
        std::unique_lock lock(shard->mtx);
        for (auto& [key, entry] : shard->store) {
            entry.release(pool_);
        }
        shard->store.clear();
        shard->used_memory_bytes = 0;
    }
}

CacheStore::Shard& CacheStore::shard_for(const std::string& key) {
    size_t idx = std::hash<std::string>{}(key) % shards_.size();
    return *shards_[idx];
}

const CacheStore::Shard& CacheStore::shard_for(const std::string& key) const {
    size_t idx = std::hash<std::string>{}(key) % shards_.size();
    return *shards_[idx];
}

size_t CacheStore::entry_memory(const std::string& key, const CacheEntry& entry) const {
    return key.size() + entry.size;
}

void CacheStore::erase_entry(Shard& shard, std::unordered_map<std::string, CacheEntry>::iterator it) {
    if (it == shard.store.end()) return;
    shard.used_memory_bytes -= entry_memory(it->first, it->second);
    it->second.release(pool_);
    shard.store.erase(it);
}

size_t CacheStore::cleanup_expired_locked(Shard& shard) {
    size_t removed = 0;
    for (auto it = shard.store.begin(); it != shard.store.end(); ) {
        if (it->second.is_expired()) {
            auto current = it++;
            erase_entry(shard, current);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t CacheStore::shard_memory_limit() const {
    const size_t max_memory = max_memory_bytes_.load(std::memory_order_relaxed);
    if (max_memory == 0) return 0;
    return (max_memory + shards_.size() - 1) / shards_.size();
}

bool CacheStore::evict_lru_until_fits(Shard& shard, const std::string& protected_key,
                                      size_t required_bytes) {
    const size_t limit = shard_memory_limit();
    while (limit > 0 && shard.used_memory_bytes + required_bytes > limit) {
        auto victim = shard.store.end();
        auto oldest = std::chrono::steady_clock::time_point::max();
        for (auto it = shard.store.begin(); it != shard.store.end(); ++it) {
            if (it->first == protected_key) continue;
            if (it->second.is_expired()) {
                victim = it;
                break;
            }
            if (it->second.last_access_time < oldest) {
                oldest = it->second.last_access_time;
                victim = it;
            }
        }
        if (victim == shard.store.end()) return false;
        erase_entry(shard, victim);
        ++shard.evicted_keys;
    }
    return true;
}

SetResult CacheStore::set(const std::string& key, const std::string& value, int ttl_seconds) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    cleanup_expired_locked(shard);

    auto it = shard.store.find(key);
    size_t old_memory = (it == shard.store.end()) ? 0 : entry_memory(it->first, it->second);
    size_t new_memory = key.size() + value.size();
    const size_t limit = shard_memory_limit();
    if (limit > 0 && new_memory > limit) {
        return SetResult::OutOfMemory;
    }

    size_t required_extra = new_memory > old_memory ? new_memory - old_memory : 0;
    if (limit > 0 && shard.used_memory_bytes + required_extra > limit) {
        if (eviction_policy_.load(std::memory_order_relaxed) == EvictionPolicy::NoEviction ||
            !evict_lru_until_fits(shard, key, required_extra)) {
            return SetResult::OutOfMemory;
        }
    }

    auto& entry = shard.store[key];
    if (old_memory > 0) {
        shard.used_memory_bytes -= old_memory;
    }
    if (!entry.setValue(value, pool_)) {
        shard.store.erase(key);
        return SetResult::AllocationFailed;
    }
    shard.used_memory_bytes += key.size() + entry.size;
    entry.last_access_time = std::chrono::steady_clock::now();
    if (ttl_seconds > 0) {
        entry.expire_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    } else {
        entry.expire_time = std::chrono::steady_clock::time_point{};
    }
    return SetResult::Ok;
}

ValueUpdateResult CacheStore::set_if_absent(const std::string& key, const std::string& value) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    cleanup_expired_locked(shard);

    if (shard.store.find(key) != shard.store.end()) {
        return ValueUpdateResult{SetResult::Ok, false, {}, 0, 0};
    }

    const size_t new_memory = key.size() + value.size();
    const size_t limit = shard_memory_limit();
    if (limit > 0 && new_memory > limit) {
        return ValueUpdateResult{SetResult::OutOfMemory, false, {}, 0, 0};
    }
    if (limit > 0 && shard.used_memory_bytes + new_memory > limit) {
        if (eviction_policy_.load(std::memory_order_relaxed) == EvictionPolicy::NoEviction ||
            !evict_lru_until_fits(shard, key, new_memory)) {
            return ValueUpdateResult{SetResult::OutOfMemory, false, {}, 0, 0};
        }
    }

    auto& entry = shard.store[key];
    if (!entry.setValue(value, pool_)) {
        shard.store.erase(key);
        return ValueUpdateResult{SetResult::AllocationFailed, false, {}, 0, 0};
    }
    shard.used_memory_bytes += key.size() + entry.size;
    entry.expire_time = std::chrono::steady_clock::time_point{};
    entry.last_access_time = std::chrono::steady_clock::now();
    return ValueUpdateResult{SetResult::Ok, true, value, entry.size, 0};
}

ValueUpdateResult CacheStore::append(const std::string& key, const std::string& suffix) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    cleanup_expired_locked(shard);

    auto it = shard.store.find(key);
    std::string new_value;
    std::chrono::steady_clock::time_point expire_time{};
    size_t old_memory = 0;
    if (it != shard.store.end()) {
        old_memory = entry_memory(it->first, it->second);
        expire_time = it->second.expire_time;
        new_value = it->second.getValue();
    }
    new_value += suffix;

    const size_t new_memory = key.size() + new_value.size();
    const size_t limit = shard_memory_limit();
    if (limit > 0 && new_memory > limit) {
        return ValueUpdateResult{SetResult::OutOfMemory, false, {}, 0, 0};
    }
    const size_t required_extra = new_memory > old_memory ? new_memory - old_memory : 0;
    if (limit > 0 && shard.used_memory_bytes + required_extra > limit) {
        if (eviction_policy_.load(std::memory_order_relaxed) == EvictionPolicy::NoEviction ||
            !evict_lru_until_fits(shard, key, required_extra)) {
            return ValueUpdateResult{SetResult::OutOfMemory, false, {}, 0, 0};
        }
        it = shard.store.find(key);
    }

    auto& entry = shard.store[key];
    if (old_memory > 0) {
        shard.used_memory_bytes -= old_memory;
    }
    if (!entry.setValue(new_value, pool_)) {
        shard.store.erase(key);
        return ValueUpdateResult{SetResult::AllocationFailed, false, {}, 0, 0};
    }
    shard.used_memory_bytes += key.size() + entry.size;
    entry.expire_time = expire_time;
    entry.last_access_time = std::chrono::steady_clock::now();
    return ValueUpdateResult{SetResult::Ok, true, new_value, entry.size, ttlSecondsForAof(entry)};
}

IncrementResult CacheStore::increment(const std::string& key, long long delta) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    cleanup_expired_locked(shard);

    auto it = shard.store.find(key);
    long long current = 0;
    std::chrono::steady_clock::time_point expire_time{};
    size_t old_memory = 0;
    if (it != shard.store.end()) {
        old_memory = entry_memory(it->first, it->second);
        expire_time = it->second.expire_time;
        if (!parseInt64Strict(it->second.getValue(), current)) {
            return IncrementResult{IncrementResultCode::NotInteger, 0, {}, 0};
        }
    }
    if (addWouldOverflow(current, delta)) {
        return IncrementResult{IncrementResultCode::Overflow, 0, {}, 0};
    }

    long long next = current + delta;
    std::string new_value = std::to_string(next);
    const size_t new_memory = key.size() + new_value.size();
    const size_t limit = shard_memory_limit();
    if (limit > 0 && new_memory > limit) {
        return IncrementResult{IncrementResultCode::OutOfMemory, 0, {}, 0};
    }
    const size_t required_extra = new_memory > old_memory ? new_memory - old_memory : 0;
    if (limit > 0 && shard.used_memory_bytes + required_extra > limit) {
        if (eviction_policy_.load(std::memory_order_relaxed) == EvictionPolicy::NoEviction ||
            !evict_lru_until_fits(shard, key, required_extra)) {
            return IncrementResult{IncrementResultCode::OutOfMemory, 0, {}, 0};
        }
    }

    auto& entry = shard.store[key];
    if (old_memory > 0) {
        shard.used_memory_bytes -= old_memory;
    }
    if (!entry.setValue(new_value, pool_)) {
        shard.store.erase(key);
        return IncrementResult{IncrementResultCode::AllocationFailed, 0, {}, 0};
    }
    shard.used_memory_bytes += key.size() + entry.size;
    entry.expire_time = expire_time;
    entry.last_access_time = std::chrono::steady_clock::now();
    return IncrementResult{IncrementResultCode::Ok, next, new_value, ttlSecondsForAof(entry)};
}

std::optional<std::string> CacheStore::get(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return std::nullopt;
    if (!it->second.is_expired()) {
        it->second.last_access_time = std::chrono::steady_clock::now();
        return it->second.getValue();
    }
    erase_entry(shard, it);
    return std::nullopt;
}

std::optional<std::string> CacheStore::get_and_delete(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return std::nullopt;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return std::nullopt;
    }
    std::string value = it->second.getValue();
    erase_entry(shard, it);
    return value;
}

std::optional<std::string> CacheStore::get_and_expire(const std::string& key, int64_t ttl_ms) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return std::nullopt;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return std::nullopt;
    }
    it->second.last_access_time = std::chrono::steady_clock::now();
    if (ttl_ms < 0) {
        it->second.expire_time = std::chrono::steady_clock::time_point{};
    } else {
        it->second.expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
    }
    return it->second.getValue();
}

bool CacheStore::persist(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return false;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return false;
    }
    if (it->second.expire_time == std::chrono::steady_clock::time_point{}) {
        it->second.last_access_time = std::chrono::steady_clock::now();
        return false;
    }
    it->second.expire_time = std::chrono::steady_clock::time_point{};
    it->second.last_access_time = std::chrono::steady_clock::now();
    return true;
}

bool CacheStore::del(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return false;
    erase_entry(shard, it);
    return true;
}

bool CacheStore::exists(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return false;
    if (!it->second.is_expired()) {
        it->second.last_access_time = std::chrono::steady_clock::now();
        return true;
    }
    erase_entry(shard, it);
    return false;
}

bool CacheStore::expire(const std::string& key, int ttl_seconds) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return false;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return false;
    }
    it->second.last_access_time = std::chrono::steady_clock::now();
    it->second.expire_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    return true;
}

bool CacheStore::pexpire(const std::string& key, int64_t ttl_ms) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return false;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return false;
    }
    it->second.last_access_time = std::chrono::steady_clock::now();
    it->second.expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
    return true;
}

long long CacheStore::ttl(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return -2;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return -2;
    }
    it->second.last_access_time = std::chrono::steady_clock::now();
    if (it->second.expire_time == std::chrono::steady_clock::time_point{}) {
        return -1;
    }
    auto remaining = it->second.expire_time - std::chrono::steady_clock::now();
    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (remaining_ms <= 0) return 0;
    return (remaining_ms + 999) / 1000;
}

long long CacheStore::pttl(const std::string& key) {
    Shard& shard = shard_for(key);
    std::unique_lock lock(shard.mtx);
    auto it = shard.store.find(key);
    if (it == shard.store.end()) return -2;
    if (it->second.is_expired()) {
        erase_entry(shard, it);
        return -2;
    }
    it->second.last_access_time = std::chrono::steady_clock::now();
    if (it->second.expire_time == std::chrono::steady_clock::time_point{}) {
        return -1;
    }
    auto remaining = it->second.expire_time - std::chrono::steady_clock::now();
    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    return remaining_ms <= 0 ? 0 : remaining_ms;
}

size_t CacheStore::cleanup() {
    size_t removed = 0;
    for (auto& shard : shards_) {
        std::unique_lock lock(shard->mtx);
        removed += cleanup_expired_locked(*shard);
    }
    return removed;
}

std::vector<std::string> CacheStore::keys() const {
    std::vector<std::string> result;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mtx);
        result.reserve(result.size() + shard->store.size());
        for (const auto& [key, entry] : shard->store) {
            if (!entry.is_expired()) result.push_back(key);
        }
    }
    return result;
}

SnapshotData CacheStore::snapshot() const {
    SnapshotData result;
    const auto now_steady = std::chrono::steady_clock::now();
    const uint64_t now_unix_ms = currentUnixMillis();
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mtx);
        for (const auto& [key, entry] : shard->store) {
            if (!entry.is_expired()) {
                uint64_t expire_at_ms = 0;
                if (entry.expire_time != std::chrono::steady_clock::time_point{}) {
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.expire_time - now_steady);
                    if (remaining.count() <= 0) continue;
                    expire_at_ms = now_unix_ms + static_cast<uint64_t>(remaining.count());
                }
                result[key] = SnapshotEntry{entry.getValue(), expire_at_ms};
            }
        }
    }
    return result;
}

void CacheStore::load_snapshot(const SnapshotData& data) {
    for (auto& shard : shards_) {
        std::unique_lock lock(shard->mtx);
        for (auto& [key, entry] : shard->store) {
            entry.release(pool_);
        }
        shard->store.clear();
        shard->used_memory_bytes = 0;
    }

    const uint64_t now_unix_ms = currentUnixMillis();
    const auto now_steady = std::chrono::steady_clock::now();
    for (const auto& [key, snapshot_entry] : data) {
        if (snapshot_entry.expire_at_ms > 0 && snapshot_entry.expire_at_ms <= now_unix_ms) {
            continue;
        }
        uint64_t remaining_ms = 0;
        if (snapshot_entry.expire_at_ms > 0) {
            remaining_ms = snapshot_entry.expire_at_ms - now_unix_ms;
            if (remaining_ms > kMaxSnapshotTtlMillis) {
                MINIREDIS_LOG_WARN("cache") << "load_snapshot ttl too large, key=" << key;
                continue;
            }
        }
        Shard& shard = shard_for(key);
        std::unique_lock lock(shard.mtx);
        const size_t limit = shard_memory_limit();
        const size_t new_memory = key.size() + snapshot_entry.value.size();
        if (limit > 0 && new_memory > limit) {
            MINIREDIS_LOG_WARN("cache") << "load_snapshot skip key over shard maxmemory, key=" << key;
            continue;
        }
        if (limit > 0 && shard.used_memory_bytes + new_memory > limit) {
            if (eviction_policy_.load(std::memory_order_relaxed) == EvictionPolicy::NoEviction ||
                !evict_lru_until_fits(shard, key, new_memory)) {
                MINIREDIS_LOG_WARN("cache") << "load_snapshot skip key due to maxmemory, key=" << key;
                continue;
            }
        }
        auto& entry = shard.store[key];
        if (!entry.setValue(snapshot_entry.value, pool_)) {
            //如果value太大会失败
            MINIREDIS_LOG_ERROR("cache") << "load_snapshot value allocation failed, key=" << key;
            shard.store.erase(key);
        } else {
            if (snapshot_entry.expire_at_ms > 0) {
                entry.expire_time = now_steady + std::chrono::milliseconds(remaining_ms);
            } else {
                entry.expire_time = {};
            }
            entry.last_access_time = now_steady;
            shard.used_memory_bytes += key.size() + entry.size;
        }
    }
}

size_t CacheStore::key_count() const {
    size_t count = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mtx);
        for (const auto& [key, entry] : shard->store) {
            if (!entry.is_expired()) ++count;
        }
    }
    return count;
}

size_t CacheStore::used_memory_bytes() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mtx);
        total += shard->used_memory_bytes;
    }
    return total;
}

size_t CacheStore::max_memory_bytes() const {
    return max_memory_bytes_.load(std::memory_order_relaxed);
}

size_t CacheStore::evicted_keys() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard->mtx);
        total += shard->evicted_keys;
    }
    return total;
}

void CacheStore::configure_memory_limit(size_t max_memory_bytes, EvictionPolicy policy) {
    max_memory_bytes_.store(max_memory_bytes, std::memory_order_relaxed);
    eviction_policy_.store(policy, std::memory_order_relaxed);
    if (max_memory_bytes > 0 && policy == EvictionPolicy::Lru) {
        for (auto& shard : shards_) {
            std::unique_lock lock(shard->mtx);
            evict_lru_until_fits(*shard, "", 0);
        }
    }
}

} // namespace miniredis
