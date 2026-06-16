#include "miniredis/persistence/persistence_manager.hpp"
#include <iostream>

namespace miniredis {

PersistenceManager::PersistenceManager(CacheStore& cache, const std::string& snapshot_file,
                                       DynamicThreadPool& pool, int interval_sec)
    : cache_(cache), file_persistence_(snapshot_file), pool_(pool),
      snapshot_interval_sec_(interval_sec), running_(false) {}

PersistenceManager::~PersistenceManager() { stop(); }

void PersistenceManager::start() {
    if (running_) return;
    running_ = true;
    if (!loadSnapshotFromFile())
        std::cerr << "Warning: failed to load snapshot from file, starting with empty cache." << std::endl;
    worker_ = std::thread(&PersistenceManager::workerLoop, this);
}

void PersistenceManager::stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    takeSnapshot(); 
}

bool PersistenceManager::takeSnapshot() { return saveSnapshotToFile(); }

void PersistenceManager::workerLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(snapshot_interval_sec_), [this]() {
            return !running_.load();
        });
        if (!running_) break;
        lock.unlock();
        //必须future阻塞等待，因为如果前一次保存很慢，用了20+秒。这样下一个快照任务会写入同一个snapshot文件
        auto future = pool_.submit([this]() { this->saveSnapshotToFile(); });
        future.get();//等待保存
    }
}

bool PersistenceManager::saveSnapshotToFile() {
    auto data = cache_.snapshot();
    std::cout << "[Persistence] Saving snapshot with " << data.size() << " keys to file..." << std::endl;
    bool ok = file_persistence_.saveSnapshot(data);
    if (ok) std::cout << "[Persistence] Snapshot saved successfully." << std::endl;
    else std::cerr << "[Persistence] Failed to save snapshot." << std::endl;
    return ok;
}

bool PersistenceManager::loadSnapshotFromFile() {
    std::unordered_map<std::string, std::string> data;
    if (!file_persistence_.loadSnapshot(data)) return false;
    std::cout << "[Persistence] Loaded " << data.size() << " keys from file." << std::endl;
    cache_.load_snapshot(data);
    return true;
}

} // namespace miniredis
