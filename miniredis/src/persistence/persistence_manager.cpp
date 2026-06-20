#include "miniredis/persistence/persistence_manager.hpp"
#include <chrono>
#include <exception>
#include <iostream>

namespace miniredis {

PersistenceManager::PersistenceManager(CacheStore& cache, const std::string& snapshot_file,
                                       DynamicThreadPool& pool, int interval_sec)
    : cache_(cache), file_persistence_(snapshot_file), pool_(pool),
      snapshot_interval_sec_(interval_sec), running_(false), snapshot_running_(false) {}

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
    while (snapshot_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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
        submitSnapshotIfIdle();
    }
}

void PersistenceManager::submitSnapshotIfIdle() {
    bool expected = false;
    if (!snapshot_running_.compare_exchange_strong(expected, true)) {
        return;
    }
    try {
        pool_.submit([this]() {
            try {
                this->saveSnapshotToFile();
            } catch (const std::exception& e) {
                std::cerr << "[Persistence] Snapshot task failed: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[Persistence] Snapshot task failed with unknown error" << std::endl;
            }
            snapshot_running_.store(false);
        });
    } catch (const std::exception& e) {
        snapshot_running_.store(false);
        std::cerr << "[Persistence] Failed to submit snapshot task: " << e.what() << std::endl;
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
