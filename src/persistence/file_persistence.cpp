#include "miniredis/persistence/file_persistence.hpp"
#include "miniredis/core/logger.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <string>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace miniredis {
namespace {

constexpr const char* kSnapshotMagicV1 = "MINIREDIS_SNAPSHOT_V1";
constexpr const char* kSnapshotMagicV2 = "MINIREDIS_SNAPSHOT_V2";
constexpr uint64_t kMaxEntrySize = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxSnapshotEntries = 1'000'000ULL;
constexpr uint64_t kMaxSnapshotBytes = 1024ULL * 1024ULL * 1024ULL;

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
        MINIREDIS_LOG_ERROR("snapshot") << "failed to open snapshot for fsync: "
                                        << path << " error=" << std::strerror(errno);
        return false;
    }
    bool ok = (::fsync(fd) == 0);
    if (!ok) {
        MINIREDIS_LOG_ERROR("snapshot") << "fsync failed for snapshot: "
                                        << path << " error=" << std::strerror(errno);
    }
    ::close(fd);
    return ok;
}

bool fsyncParentDirectory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        parent = ".";
    }

    int fd = ::open(parent.string().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        MINIREDIS_LOG_WARN("snapshot") << "failed to open snapshot parent directory for fsync: "
                                       << parent.string() << " error=" << std::strerror(errno);
        return false;
    }
    bool ok = (::fsync(fd) == 0);
    if (!ok) {
        MINIREDIS_LOG_WARN("snapshot") << "fsync failed for snapshot parent directory: "
                                       << parent.string() << " error=" << std::strerror(errno);
    }
    ::close(fd);
    return ok;
}

std::string badSnapshotPath(const std::string& path) {
    return path + ".bad";
}

std::string backupSnapshotPath(const std::string& path) {
    return path + ".bak";
}

void markBadSnapshot(const std::string& path) {
    if (!std::filesystem::exists(path)) return;

    const std::string bad_path = badSnapshotPath(path);
    std::error_code ec;
    std::filesystem::remove(bad_path, ec);
    ec.clear();
    std::filesystem::rename(path, bad_path, ec);
    if (ec) {
        MINIREDIS_LOG_WARN("snapshot") << "failed to move bad snapshot " << path
                                       << " to " << bad_path << ": " << ec.message();
        return;
    }
    fsyncParentDirectory(path);
    MINIREDIS_LOG_WARN("snapshot") << "moved bad snapshot to " << bad_path;
}

bool loadLegacyTextSnapshot(const std::string& path,
                            SnapshotData& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;

    std::string line;
    size_t count = 0;
    SnapshotData loaded;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        size_t space = line.find(' ');
        if (space == std::string::npos || space == 0) {
            MINIREDIS_LOG_ERROR("snapshot") << "invalid legacy snapshot line";
            return false;
        }
        const size_t key_size = space;
        const size_t value_size = line.size() - space - 1;
        if (key_size > kMaxEntrySize || value_size > kMaxEntrySize ||
            count >= kMaxSnapshotEntries) {
            MINIREDIS_LOG_ERROR("snapshot") << "legacy snapshot limit exceeded";
            return false;
        }
        loaded[line.substr(0, space)] = SnapshotEntry{line.substr(space + 1), 0};
        ++count;
    }
    if (ifs.bad()) return false;
    out = std::move(loaded);
    MINIREDIS_LOG_INFO("snapshot") << "loaded legacy snapshot with " << count
                                   << " keys from " << path;
    return true;
}

bool loadSnapshotFile(const std::string& path, SnapshotData& out) {
    std::error_code size_ec;
    if (!std::filesystem::exists(path, size_ec)) {
        return !size_ec;
    }
    const uintmax_t file_size = std::filesystem::file_size(path, size_ec);
    if (size_ec || file_size == 0 || file_size > kMaxSnapshotBytes) {
        MINIREDIS_LOG_ERROR("snapshot") << "invalid snapshot file size: " << path;
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }

    std::string magic;
    if (!std::getline(ifs, magic)) {
        MINIREDIS_LOG_ERROR("snapshot") << "missing snapshot header: " << path;
        return false;
    }
    const bool is_v1 = (magic == kSnapshotMagicV1);
    const bool is_v2 = (magic == kSnapshotMagicV2);
    if (!is_v1 && !is_v2) {
        if (magic.rfind("MINIREDIS_SNAPSHOT_", 0) == 0) {
            MINIREDIS_LOG_ERROR("snapshot") << "unsupported snapshot version: " << magic;
            return false;
        }
        ifs.close();
        return loadLegacyTextSnapshot(path, out);
    }

    uint64_t count = 0;
    if (!readU64(ifs, count)) {
        MINIREDIS_LOG_ERROR("snapshot") << "invalid snapshot header: " << path;
        return false;
    }
    if (count > kMaxSnapshotEntries ||
        count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        MINIREDIS_LOG_ERROR("snapshot") << "snapshot entry count too large: " << count;
        return false;
    }

    SnapshotData loaded;
    loaded.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key_size = 0;
        uint64_t value_size = 0;
        uint64_t expire_at_ms = 0;
        if (!readU64(ifs, key_size) || !readU64(ifs, value_size) ||
            (is_v2 && !readU64(ifs, expire_at_ms))) {
            MINIREDIS_LOG_ERROR("snapshot") << "invalid snapshot entry header at index " << i;
            return false;
        }
        if (key_size > kMaxEntrySize || value_size > kMaxEntrySize ||
            key_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            value_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            MINIREDIS_LOG_ERROR("snapshot") << "snapshot entry too large at index " << i;
            return false;
        }

        std::string key(static_cast<size_t>(key_size), '\0');
        std::string value(static_cast<size_t>(value_size), '\0');
        if (key_size > 0) ifs.read(key.data(), static_cast<std::streamsize>(key.size()));
        if (value_size > 0) ifs.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (!ifs) {
            MINIREDIS_LOG_ERROR("snapshot") << "invalid snapshot entry body at index " << i;
            return false;
        }
        loaded[std::move(key)] = SnapshotEntry{std::move(value), expire_at_ms};
    }
    if (ifs.peek() != std::char_traits<char>::eof()) {
        MINIREDIS_LOG_ERROR("snapshot") << "unexpected trailing snapshot data: " << path;
        return false;
    }

    out = std::move(loaded);
    MINIREDIS_LOG_INFO("snapshot") << "loaded snapshot with " << out.size()
                                   << " keys from " << path;
    return true;
}

} // namespace

FilePersistence::FilePersistence(const std::string& filepath) : filepath_(filepath) {}

bool FilePersistence::saveSnapshot(const SnapshotData& data) {
    if (data.size() > kMaxSnapshotEntries) {
        MINIREDIS_LOG_ERROR("snapshot") << "snapshot has too many entries: " << data.size();
        return false;
    }

    uint64_t encoded_size = std::strlen(kSnapshotMagicV2) + 1 + sizeof(uint64_t);
    for (const auto& [key, entry] : data) {
        if (key.size() > kMaxEntrySize || entry.value.size() > kMaxEntrySize) {
            MINIREDIS_LOG_ERROR("snapshot") << "snapshot entry exceeds size limit";
            return false;
        }
        constexpr uint64_t kEntryHeaderBytes = sizeof(uint64_t) * 3;
        const uint64_t payload_size = static_cast<uint64_t>(key.size()) +
                                      static_cast<uint64_t>(entry.value.size());
        if (payload_size > kMaxSnapshotBytes - kEntryHeaderBytes ||
            encoded_size > kMaxSnapshotBytes - kEntryHeaderBytes - payload_size) {
            MINIREDIS_LOG_ERROR("snapshot") << "snapshot exceeds total size limit";
            return false;
        }
        encoded_size += kEntryHeaderBytes + payload_size;
    }

    const std::string tmp_path = filepath_ + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            MINIREDIS_LOG_ERROR("snapshot") << "failed to open snapshot temp file for write: "
                                            << tmp_path;
            return false;
        }

        ofs << kSnapshotMagicV2 << '\n';
        if (!writeU64(ofs, static_cast<uint64_t>(data.size()))) {
            MINIREDIS_LOG_ERROR("snapshot") << "failed to write snapshot header: " << tmp_path;
            return false;
        }

        for (const auto& [key, entry] : data) {
            const auto& value = entry.value;
            if (!writeU64(ofs, static_cast<uint64_t>(key.size())) ||
                !writeU64(ofs, static_cast<uint64_t>(value.size())) ||
                !writeU64(ofs, entry.expire_at_ms)) {
                MINIREDIS_LOG_ERROR("snapshot") << "failed to write snapshot entry header";
                return false;
            }
            ofs.write(key.data(), static_cast<std::streamsize>(key.size()));
            ofs.write(value.data(), static_cast<std::streamsize>(value.size()));
            if (!ofs) {
                MINIREDIS_LOG_ERROR("snapshot") << "failed to write snapshot entry body";
                return false;
            }
        }
        ofs.flush();
        if (!ofs) {
            MINIREDIS_LOG_ERROR("snapshot") << "failed to flush snapshot temp file: " << tmp_path;
            return false;
        }
    }

    if (!fsyncFile(tmp_path)) return false;
    if (std::filesystem::exists(filepath_)) {
        std::error_code ec;
        std::filesystem::copy_file(filepath_, backupSnapshotPath(filepath_),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            MINIREDIS_LOG_WARN("snapshot") << "failed to update snapshot backup: " << ec.message();
        } else {
            fsyncFile(backupSnapshotPath(filepath_));
        }
    }
    if (::rename(tmp_path.c_str(), filepath_.c_str()) != 0) {
        MINIREDIS_LOG_ERROR("snapshot") << "failed to replace snapshot file: "
                                        << filepath_ << " error=" << std::strerror(errno);
        return false;
    }
    fsyncParentDirectory(filepath_);

    MINIREDIS_LOG_INFO("snapshot") << "saved snapshot with " << data.size()
                                   << " keys to " << filepath_;
    return true;
}

bool FilePersistence::loadSnapshot(SnapshotData& out) {
    std::error_code exists_ec;
    const bool primary_exists = std::filesystem::exists(filepath_, exists_ec);
    if (exists_ec) return false;
    if (primary_exists) {
        if (loadSnapshotFile(filepath_, out)) return true;
        markBadSnapshot(filepath_);
    }

    const std::string backup_path = backupSnapshotPath(filepath_);
    if (std::filesystem::exists(backup_path)) {
        MINIREDIS_LOG_WARN("snapshot") << "trying backup snapshot: " << backup_path;
        if (loadSnapshotFile(backup_path, out)) {
            return true;
        }
        markBadSnapshot(backup_path);
    }
    return !primary_exists;
}

} // namespace miniredis
