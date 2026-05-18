#include "consistent_hash.hpp"
#include <sstream>

namespace miniredis {

static size_t defaultHash(const std::string& s) {
    std::hash<std::string> h;
    return h(s);
}

ConsistentHash::ConsistentHash(size_t virtual_nodes, HashFunc hash_func)
    : virtual_nodes_(virtual_nodes), hash_func_(hash_func ? hash_func : defaultHash) {}

void ConsistentHash::AddNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < virtual_nodes_; ++i) {
        std::ostringstream oss;
        oss << node << "#" << i;
        size_t h = hash(oss.str());
        ring_[h] = node;
    }
}

void ConsistentHash::RemoveNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < virtual_nodes_; ++i) {
        std::ostringstream oss;
        oss << node << "#" << i;
        size_t h = hash(oss.str());
        ring_.erase(h);
    }
}

std::string ConsistentHash::GetNode(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) return "";
    size_t h = hash(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();
    return it->second;
}

std::vector<std::string> ConsistentHash::GetAllNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> nodes;
    for (const auto& [h, node] : ring_) {
        if (nodes.empty() || nodes.back() != node) nodes.push_back(node);
    }
    return nodes;
}

size_t ConsistentHash::hash(const std::string& s) const {
    return hash_func_(s);
}

void ConsistentHash::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
}

}