#include "persistence_manager.hpp"
#include <iostream>

namespace miniredis {

PersistenceManager::PersistenceManager(CacheStore& cache, MySQLClient& mysql,
                                       DynamicThreadPool& pool, int interval)
    : cache_(cache), mysql_(mysql), pool_(pool),
      snapshot_interval_sec_(interval), running_(false) {}

PersistenceManager::~PersistenceManager() { stop(); }

void PersistenceManager::start() {
    if (running_) return;
    running_ = true;
    if (!loadSnapshotFromDB())
        std::cerr << "Warning: failed to load snapshot from MySQL, starting with empty cache." << std::endl;
    worker_ = std::thread(&PersistenceManager::workerLoop, this);
}

void PersistenceManager::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_.joinable()) worker_.join();
    takeSnapshot();
}

bool PersistenceManager::takeSnapshot() { return saveSnapshotToDB(); }

void PersistenceManager::workerLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(snapshot_interval_sec_));
        if (!running_) break;
        auto future = pool_.submit([this]() { this->saveSnapshotToDB(); });
        future.get();
    }
}

bool PersistenceManager::saveSnapshotToDB() {
    auto data = cache_.snapshot();
    if (data.empty()) return true;
    std::cout << "[Persistence] Saving snapshot with " << data.size() << " keys to MySQL..." << std::endl;
    bool ok = mysql_.saveSnapshot(data);
    if (ok) std::cout << "[Persistence] Snapshot saved successfully." << std::endl;
    else std::cerr << "[Persistence] Failed to save snapshot." << std::endl;
    return ok;
}

bool PersistenceManager::loadSnapshotFromDB() {
    std::unordered_map<std::string, std::string> data;
    if (!mysql_.loadAll(data)) return false;
    std::cout << "[Persistence] Loaded " << data.size() << " keys from MySQL." << std::endl;
    cache_.load_snapshot(data);
    return true;
}

}