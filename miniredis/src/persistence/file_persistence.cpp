#include "miniredis/persistence/file_persistence.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace miniredis {
namespace {

constexpr const char* kSnapshotMagic = "MINIREDIS_SNAPSHOT_V1";
constexpr uint64_t kMaxEntrySize = 512ULL * 1024ULL * 1024ULL;

bool writeU64(std::ostream& os, uint64_t value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(os);
}

bool readU64(std::istream& is, uint64_t& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(is);
}

bool fsyncFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open snapshot for fsync: " << path
                  << " error=" << std::strerror(errno) << std::endl;
        return false;
    }
    bool ok = (::fsync(fd) == 0);
    if (!ok) {
        std::cerr << "fsync failed for snapshot: " << path
                  << " error=" << std::strerror(errno) << std::endl;
    }
    ::close(fd);
    return ok;
}

bool loadLegacyTextSnapshot(const std::string& path,
                            std::unordered_map<std::string, std::string>& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;

    std::string line;
    size_t count = 0;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            std::cerr << "Invalid legacy snapshot line: " << line << std::endl;
            continue;
        }
        out[line.substr(0, space)] = line.substr(space + 1);
        ++count;
    }
    std::cout << "[FilePersistence] Loaded legacy snapshot with " << count
              << " keys from " << path << std::endl;
    return true;
}

} // namespace

FilePersistence::FilePersistence(const std::string& filepath) : filepath_(filepath) {}

bool FilePersistence::saveSnapshot(const std::unordered_map<std::string, std::string>& data) {
    const std::string tmp_path = filepath_ + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "Failed to open snapshot temp file for write: "
                      << tmp_path << std::endl;
            return false;
        }

        ofs << kSnapshotMagic << '\n';
        if (!writeU64(ofs, static_cast<uint64_t>(data.size()))) {
            std::cerr << "Failed to write snapshot header: " << tmp_path << std::endl;
            return false;
        }

        for (const auto& [key, value] : data) {
            if (!writeU64(ofs, static_cast<uint64_t>(key.size())) ||
                !writeU64(ofs, static_cast<uint64_t>(value.size()))) {
                std::cerr << "Failed to write snapshot entry header" << std::endl;
                return false;
            }
            ofs.write(key.data(), static_cast<std::streamsize>(key.size()));
            ofs.write(value.data(), static_cast<std::streamsize>(value.size()));
            if (!ofs) {
                std::cerr << "Failed to write snapshot entry body" << std::endl;
                return false;
            }
        }
        ofs.flush();
        if (!ofs) {
            std::cerr << "Failed to flush snapshot temp file: " << tmp_path << std::endl;
            return false;
        }
    }

    if (!fsyncFile(tmp_path)) return false;
    if (::rename(tmp_path.c_str(), filepath_.c_str()) != 0) {
        std::cerr << "Failed to replace snapshot file: " << filepath_
                  << " error=" << std::strerror(errno) << std::endl;
        return false;
    }

    std::cout << "[FilePersistence] Saved snapshot with " << data.size()
              << " keys to " << filepath_ << std::endl;
    return true;
}

bool FilePersistence::loadSnapshot(std::unordered_map<std::string, std::string>& out) {
    std::ifstream ifs(filepath_, std::ios::binary);
    if (!ifs) {
        return true;
    }

    std::string magic;
    if (!std::getline(ifs, magic)) {
        return true;
    }
    if (magic != kSnapshotMagic) {
        ifs.close();
        return loadLegacyTextSnapshot(filepath_, out);
    }

    uint64_t count = 0;
    if (!readU64(ifs, count)) {
        std::cerr << "Invalid snapshot header: " << filepath_ << std::endl;
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    loaded.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key_size = 0;
        uint64_t value_size = 0;
        if (!readU64(ifs, key_size) || !readU64(ifs, value_size)) {
            std::cerr << "Invalid snapshot entry header at index " << i << std::endl;
            return false;
        }
        if (key_size > kMaxEntrySize || value_size > kMaxEntrySize ||
            key_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            value_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            std::cerr << "Snapshot entry too large at index " << i << std::endl;
            return false;
        }

        std::string key(static_cast<size_t>(key_size), '\0');
        std::string value(static_cast<size_t>(value_size), '\0');
        if (key_size > 0) ifs.read(key.data(), static_cast<std::streamsize>(key.size()));
        if (value_size > 0) ifs.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (!ifs) {
            std::cerr << "Invalid snapshot entry body at index " << i << std::endl;
            return false;
        }
        loaded[std::move(key)] = std::move(value);
    }

    out = std::move(loaded);
    std::cout << "[FilePersistence] Loaded snapshot with " << out.size()
              << " keys from " << filepath_ << std::endl;
    return true;
}

} // namespace miniredis
