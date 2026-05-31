#include "file_persistence.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace miniredis {

FilePersistence::FilePersistence(const std::string& filepath) : filepath_(filepath) {}

bool FilePersistence::saveSnapshot(const std::unordered_map<std::string, std::string>& data) {
    std::ofstream ofs(filepath_, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to open snapshot file for write: " << filepath_ << std::endl;
        return false;
    }
    // 简单格式：每行 "key value"（注意 key 和 value 中不能有换行符，实际应用需转义，这里简化）
    for (const auto& [key, value] : data) {
        ofs << key << " " << value << "\n";
        if (!ofs) {
            std::cerr << "Failed to write snapshot entry" << std::endl;
            return false;
        }
    }
    std::cout << "[FilePersistence] Saved snapshot with " << data.size() << " keys to " << filepath_ << std::endl;
    return true;
}

bool FilePersistence::loadSnapshot(std::unordered_map<std::string, std::string>& out) {
    std::ifstream ifs(filepath_);
    if (!ifs) {
        // 文件不存在是正常情况
        return true;
    }
    std::string line;
    size_t count = 0;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            std::cerr << "Invalid snapshot line: " << line << std::endl;
            continue;
        }
        std::string key = line.substr(0, space);
        std::string value = line.substr(space + 1);
        out[key] = value;
        ++count;
    }
    std::cout << "[FilePersistence] Loaded snapshot with " << count << " keys from " << filepath_ << std::endl;
    return true;
}

} // namespace miniredis