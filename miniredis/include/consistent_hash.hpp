#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

namespace miniredis {

class ConsistentHash {
public:
    using HashFunc = std::function<size_t(const std::string&)>;

    ConsistentHash(size_t virtual_nodes = 150, HashFunc hash_func = nullptr);
    void AddNode(const std::string& node);
    void RemoveNode(const std::string& node);
    std::string GetNode(const std::string& key) const;
    std::vector<std::string> GetAllNodes() const;
    void clear();

private:
    size_t virtual_nodes_;
    HashFunc hash_func_;
    std::map<size_t, std::string> ring_;
    mutable std::mutex mutex_;
    size_t hash(const std::string& s) const;
};

}