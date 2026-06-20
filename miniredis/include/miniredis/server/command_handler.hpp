#pragma once

#include "miniredis/cluster/slot_map.hpp"
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
                   ClusterSlotMap* slot_map, std::mutex* slot_map_mutex);

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
    ClusterSlotMap* slot_map_;
    std::mutex* slot_map_mutex_;
};

} // namespace miniredis
