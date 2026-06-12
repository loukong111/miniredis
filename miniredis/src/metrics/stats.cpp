#include "miniredis/metrics/stats.hpp"
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

void Stats::recordConnectionOpen() {
    connected_clients_.fetch_add(1, std::memory_order_relaxed);
    total_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::recordConnectionClose() {
    size_t current = connected_clients_.load(std::memory_order_relaxed);
    while (current > 0 &&
           !connected_clients_.compare_exchange_weak(current, current - 1,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {}
}

void Stats::recordRejectedConnection() {
    rejected_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::recordCommandLatency(size_t latency_us) {
    latency_samples_.fetch_add(1, std::memory_order_relaxed);
    total_command_latency_us_.fetch_add(latency_us, std::memory_order_relaxed);

    size_t current_max = max_command_latency_us_.load(std::memory_order_relaxed);
    while (latency_us > current_max &&
           !max_command_latency_us_.compare_exchange_weak(current_max, latency_us,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {}
}

void Stats::setMemoryPoolUsed(size_t used_blocks, size_t free_blocks) {
    mem_pool_used_.store(used_blocks, std::memory_order_relaxed);//宽松模式，延迟可接收，追求速度！
    mem_pool_free_.store(free_blocks, std::memory_order_relaxed);
}

void Stats::setNodeAddr(const std::string& addr) {
    std::lock_guard<std::mutex> lock(node_mutex_);
    node_addr_ = addr;
}

std::string Stats::getNodeAddr() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return node_addr_;
}

std::string Stats::toJson() const {
    std::string node_addr = getNodeAddr();
    size_t latency_samples = latency_samples_.load(std::memory_order_relaxed);
    size_t avg_latency_us = 0;
    if (latency_samples > 0) {
        avg_latency_us = total_command_latency_us_.load(std::memory_order_relaxed) / latency_samples;
    }
    std::ostringstream oss;
    oss << "{";
    oss << "\"node_addr\":\"" << node_addr << "\",";
    oss << "\"total_commands\":" << total_commands_.load() << ",";
    oss << "\"get_hits\":" << get_hits_.load() << ",";
    oss << "\"get_misses\":" << get_misses_.load() << ",";
    oss << "\"key_count\":" << key_count_.load() << ",";
    oss << "\"mem_pool_used_blocks\":" << mem_pool_used_.load() << ",";
    oss << "\"mem_pool_free_blocks\":" << mem_pool_free_.load() << ",";
    oss << "\"connected_clients\":" << connected_clients_.load() << ",";
    oss << "\"total_connections\":" << total_connections_.load() << ",";
    oss << "\"rejected_connections\":" << rejected_connections_.load() << ",";
    oss << "\"latency_samples\":" << latency_samples << ",";
    oss << "\"avg_command_latency_us\":" << avg_latency_us << ",";
    oss << "\"max_command_latency_us\":" << max_command_latency_us_.load();
    oss << "}";
    return oss.str();
}

}
