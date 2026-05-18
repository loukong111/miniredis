#include "stats.hpp"
#include <sstream>
#include <chrono>
#include <iomanip>

namespace miniredis {

Stats& Stats::instance() {
    static Stats inst;
    return inst;
}

void Stats::recordCommand(const std::string& cmd, bool hit) {
    total_commands_++;
    if (cmd == "GET") {
        if (hit) get_hits_++;
        else get_misses_++;
    }
}

void Stats::recordSet() {
    total_commands_++;
}

void Stats::recordGetHit() {
    total_commands_++;
    get_hits_++;
}

void Stats::recordGetMiss() {
    total_commands_++;
    get_misses_++;
}

void Stats::setMemoryPoolUsed(size_t used_blocks, size_t free_blocks) {
    mem_pool_used_.store(used_blocks, std::memory_order_relaxed);
    mem_pool_free_.store(free_blocks, std::memory_order_relaxed);
}

std::string Stats::toJson() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"node_addr\":\"" << node_addr_ << "\",";
    oss << "\"total_commands\":" << total_commands_.load() << ",";
    oss << "\"get_hits\":" << get_hits_.load() << ",";
    oss << "\"get_misses\":" << get_misses_.load() << ",";
    oss << "\"key_count\":" << key_count_.load() << ",";
    oss << "\"mem_pool_used_blocks\":" << mem_pool_used_.load() << ",";
    oss << "\"mem_pool_free_blocks\":" << mem_pool_free_.load();
    oss << "}";
    return oss.str();
}

}