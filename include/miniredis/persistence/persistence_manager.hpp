#pragma once

#include "miniredis/core/cache_store.hpp"
#include "miniredis/persistence/file_persistence.hpp"
#include "miniredis/core/thread_pool.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace miniredis {

class PersistenceManager {
public:
    PersistenceManager(CacheStore& cache, const std::string& snapshot_file, 
        DynamicThreadPool& pool, int interval_sec = 20);
    ~PersistenceManager();

    void start();
    void stop();
    bool takeSnapshot();

private:
    void workerLoop();
    void submitSnapshotIfIdle();
    bool saveSnapshotToFile();
    bool loadSnapshotFromFile();

    CacheStore& cache_;
    //FilePersistence 很轻，只需要一个文件路径。所以没必要引用
    FilePersistence file_persistence_;
    DynamicThreadPool& pool_;
    int snapshot_interval_sec_;
    std::atomic<bool> running_;
    std::atomic<bool> snapshot_running_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace miniredis
