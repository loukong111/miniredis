#pragma once

#include "cache_store.hpp"
#include "mysql_client.hpp"
#include "thread_pool.hpp"
#include <atomic>
#include <thread>

namespace miniredis {

class PersistenceManager {
public:
    PersistenceManager(CacheStore& cache, MySQLClient& mysql, DynamicThreadPool& pool,
                       int snapshot_interval_sec = 20);
    ~PersistenceManager();

    void start();
    void stop();
    bool takeSnapshot();

private:
    void workerLoop();
    bool saveSnapshotToDB();
    bool loadSnapshotFromDB();

    CacheStore& cache_;
    MySQLClient& mysql_;
    DynamicThreadPool& pool_;
    int snapshot_interval_sec_;
    std::atomic<bool> running_;
    std::thread worker_;
};

}