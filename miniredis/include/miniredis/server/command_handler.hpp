#pragma once

#include "miniredis/cluster/consistent_hash.hpp"
#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/net/resp_parser.hpp"
#include "miniredis/server/config.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace miniredis {

class CommandHandler {
public:
    CommandHandler(CacheStore& cache, FixedMemoryPool& memory_pool, const AppConfig& config,
                   bool cluster_mode, std::string current_node,
                   ConsistentHash* hash_ring, std::mutex* hash_ring_mutex);

    std::string handle(const RespValue& cmd, bool& authenticated);
    void refreshRuntimeStats() const;

private:
    std::string routeIfNeeded(const std::string& cmd_name, const RespValue& cmd) const;
    std::string handleClusterCommand(const RespValue& cmd) const;
    std::vector<std::string> clusterNodes() const;

    CacheStore& cache_;
    FixedMemoryPool& memory_pool_;
    const AppConfig& config_;
    bool cluster_mode_;
    std::string current_node_;
    ConsistentHash* hash_ring_;
    std::mutex* hash_ring_mutex_;
};

} // namespace miniredis
