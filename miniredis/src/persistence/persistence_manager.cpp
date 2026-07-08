#include "miniredis/persistence/persistence_manager.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/metrics/stats.hpp"
#include <chrono>
#include <exception>

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
        MINIREDIS_LOG_WARN("persistence") << "failed to load snapshot from file, starting with empty cache";
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
                MINIREDIS_LOG_ERROR("persistence") << "snapshot task failed: " << e.what();
            } catch (...) {
                MINIREDIS_LOG_ERROR("persistence") << "snapshot task failed with unknown error";
            }
            snapshot_running_.store(false);
        });
    } catch (const std::exception& e) {
        snapshot_running_.store(false);
        MINIREDIS_LOG_ERROR("persistence") << "failed to submit snapshot task: " << e.what();
    }
}

bool PersistenceManager::saveSnapshotToFile() {
    auto started = std::chrono::steady_clock::now();
    Stats::instance().setSnapshotRunning(true);
    SnapshotData data;
    bool ok = false;
    try {
        data = cache_.snapshot();
        MINIREDIS_LOG_INFO("persistence") << "saving snapshot with " << data.size() << " keys";
        ok = file_persistence_.saveSnapshot(data);
    } catch (const std::exception& e) {
        MINIREDIS_LOG_ERROR("persistence") << "snapshot failed: " << e.what();
    } catch (...) {
        MINIREDIS_LOG_ERROR("persistence") << "snapshot failed with unknown error";
    }
    size_t duration_ms = static_cast<size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    Stats::instance().recordSnapshotResult(ok, data.size(), duration_ms);
    Stats::instance().setSnapshotRunning(false);
    if (ok) MINIREDIS_LOG_INFO("persistence") << "snapshot saved successfully";
    else MINIREDIS_LOG_ERROR("persistence") << "failed to save snapshot";
    return ok;
}

bool PersistenceManager::loadSnapshotFromFile() {
    SnapshotData data;
    if (!file_persistence_.loadSnapshot(data)) return false;
    MINIREDIS_LOG_INFO("persistence") << "loaded " << data.size() << " keys from file";
    cache_.load_snapshot(data);
    return true;
}

} // namespace miniredis
