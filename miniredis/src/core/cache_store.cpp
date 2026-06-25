#include "miniredis/core/cache_store.hpp"
#include <mutex>
#include <cstring>
#include <iostream>
#include <new>

namespace miniredis {

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

CacheStore::CacheStore(FixedMemoryPool& pool) : pool_(pool) {}

CacheStore::~CacheStore() {
    for (auto& [key, entry] : store_) {
        entry.release(pool_);
    }
}

void CacheStore::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::unique_lock lock(mtx_);
    auto& entry = store_[key];
    if (!entry.setValue(value, pool_)) {
        store_.erase(key);
        return;
    }
    if (ttl_seconds > 0) {
        entry.expire_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    } else {
        entry.expire_time = std::chrono::steady_clock::time_point{};
    }
}

std::optional<std::string> CacheStore::get(const std::string& key) {
    {
        std::shared_lock lock(mtx_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        if (!it->second.is_expired()) return it->second.getValue();
    }
    erase_if_expired(key);
    return std::nullopt;
}

bool CacheStore::del(const std::string& key) {
    std::unique_lock lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;
    it->second.release(pool_);
    store_.erase(it);
    return true;
}

bool CacheStore::exists(const std::string& key) {
    {
        std::shared_lock lock(mtx_);
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        if (!it->second.is_expired()) return true;
    }
    erase_if_expired(key);
    return false;
}

bool CacheStore::expire(const std::string& key, int ttl_seconds) {
    std::unique_lock lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;
    if (it->second.is_expired()) {
        it->second.release(pool_);
        store_.erase(it);
        return false;
    }
    it->second.expire_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    return true;
}

long long CacheStore::ttl(const std::string& key) {
    {
        std::shared_lock lock(mtx_);
        auto it = store_.find(key);
        if (it == store_.end()) return -2;
        if (it->second.is_expired()) {
            // Fall through to lazy deletion below.
        } else if (it->second.expire_time == std::chrono::steady_clock::time_point{}) {
            return -1;
        } else {
            auto remaining = it->second.expire_time - std::chrono::steady_clock::now();
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (remaining_ms <= 0) return 0;
            return (remaining_ms + 999) / 1000;
        }
    }
    erase_if_expired(key);
    return -2;
}

bool CacheStore::erase_if_expired(const std::string& key) {
    std::unique_lock lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end() || !it->second.is_expired()) return false;
    it->second.release(pool_);
    store_.erase(it);
    return true;
}

size_t CacheStore::cleanup() {
    std::unique_lock lock(mtx_);
    size_t removed = 0;
    for (auto it = store_.begin(); it != store_.end(); ) {
        if (it->second.is_expired()) {
            it->second.release(pool_);
            it = store_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<std::string> CacheStore::keys() const {
    std::shared_lock lock(mtx_);
    std::vector<std::string> result;
    result.reserve(store_.size());
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired()) result.push_back(key);
    }
    return result;
}

std::unordered_map<std::string, std::string> CacheStore::snapshot() const {
    std::shared_lock lock(mtx_);
    std::unordered_map<std::string, std::string> result;
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired()) {
            result[key] = entry.getValue();
        }
    }
    return result;
}

void CacheStore::load_snapshot(const std::unordered_map<std::string, std::string>& data) {
    std::unique_lock lock(mtx_);
    // 先释放对象中的内存
    for (auto& [key, entry] : store_) {
        entry.release(pool_);
    }
    //再删除对象
    store_.clear();
    for (const auto& [key, value] : data) {
        auto& entry = store_[key];
        if (!entry.setValue(value, pool_)) {
            //如果value太大会失败
            std::cerr << "load_snapshot:value too large , key=" << key << std::endl;
            store_.erase(key);
        } else {
            entry.expire_time = {};
        }
    }
}

size_t CacheStore::key_count() const {
    std::shared_lock lock(mtx_);
    size_t count = 0;
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired()) ++count;
    }
    return count;
}

} // namespace miniredis
