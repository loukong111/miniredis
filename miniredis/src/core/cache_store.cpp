#include "miniredis/core/cache_store.hpp"
#include <mutex>
#include <cstring>
#include <iostream>

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
        data = new char[size];
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
    is_pool_allocated = true; //默认为true，不写也行
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
    std::shared_lock lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return std::nullopt;
    if (it->second.is_expired()) return std::nullopt;
    return it->second.getValue();
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
    std::shared_lock lock(mtx_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;
    return !it->second.is_expired();
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