#pragma once

#include <string>
#include <unordered_map>

namespace miniredis {

class FilePersistence {
public:
    explicit FilePersistence(const std::string& filepath);
    bool saveSnapshot(const std::unordered_map<std::string, std::string>& data);
    bool loadSnapshot(std::unordered_map<std::string, std::string>& out);

private:
    std::string filepath_;
};

} // namespace miniredis