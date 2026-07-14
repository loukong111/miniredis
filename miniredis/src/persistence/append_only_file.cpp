#include "miniredis/persistence/append_only_file.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/metrics/stats.hpp"
#include "miniredis/net/resp_parser.hpp"
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace miniredis {
namespace {

constexpr uint64_t kMaxAofBytes = 1024ULL * 1024ULL * 1024ULL;

std::string toUpper(std::string value) {
    for (char& ch : value) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return value;
}

bool parseU64(const std::string& value, uint64_t& out) {
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    return ec == std::errc() && ptr == value.data() + value.size();
}

} // namespace

bool parseAppendFsyncPolicy(const std::string& value, AppendFsyncPolicy& policy) {
    std::string normalized = toUpper(value);
    if (normalized == "NO") {
        policy = AppendFsyncPolicy::No;
        return true;
    }
    if (normalized == "EVERYSEC") {
        policy = AppendFsyncPolicy::EverySec;
        return true;
    }
    if (normalized == "ALWAYS") {
        policy = AppendFsyncPolicy::Always;
        return true;
    }
    return false;
}

std::string appendFsyncPolicyName(AppendFsyncPolicy policy) {
    switch (policy) {
        case AppendFsyncPolicy::No: return "no";
        case AppendFsyncPolicy::EverySec: return "everysec";
        case AppendFsyncPolicy::Always: return "always";
    }
    return "everysec";
}

AppendOnlyFile::AppendOnlyFile(std::string path, AppendFsyncPolicy policy,
                               size_t rewrite_buffer_limit_bytes)
    : path_(std::move(path)),
      policy_(policy),
      rewrite_buffer_limit_bytes_(rewrite_buffer_limit_bytes),
      last_fsync_(std::chrono::steady_clock::now()) {}

AppendOnlyFile::~AppendOnlyFile() {
    close();
}

bool AppendOnlyFile::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) return true;

    std::filesystem::path parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            MINIREDIS_LOG_ERROR("aof") << "failed to create AOF directory "
                                      << parent.string() << ": " << ec.message();
            return false;
        }
    }

    cleanupRewriteTempLocked();

    fd_ = ::open(path_.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        MINIREDIS_LOG_ERROR("aof") << "failed to open AOF: " << path_
                                   << " error=" << std::strerror(errno);
        return false;
    }
    MINIREDIS_LOG_INFO("aof") << "AOF enabled: " << path_
                              << ", appendfsync=" << appendFsyncPolicyName(policy_);
    return true;
}

void AppendOnlyFile::close() {
    waitForRewrite();

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return;
    if (policy_ != AppendFsyncPolicy::No) {
        fsyncLocked();
    }
    ::close(fd_);
    fd_ = -1;
}

bool AppendOnlyFile::appendSet(const std::string& key, const std::string& value, int ttl_seconds) {
    std::vector<std::string> parts{"SET", key, value};
    if (ttl_seconds > 0) {
        parts.push_back("PXAT");
        parts.push_back(std::to_string(expireAtMillis(ttl_seconds)));
    }
    return appendCommand(parts);
}

bool AppendOnlyFile::appendDel(const std::vector<std::string>& keys) {
    if (keys.empty()) return true;
    std::vector<std::string> parts;
    parts.reserve(keys.size() + 1);
    parts.push_back("DEL");
    for (const auto& key : keys) {
        parts.push_back(key);
    }
    return appendCommand(parts);
}

bool AppendOnlyFile::appendExpire(const std::string& key, int ttl_seconds) {
    return appendCommand({"PEXPIREAT", key, std::to_string(expireAtMillis(ttl_seconds))});
}

bool AppendOnlyFile::appendPExpire(const std::string& key, int64_t ttl_ms) {
    const uint64_t now = currentUnixMillis();
    const uint64_t expire_at = now + static_cast<uint64_t>(ttl_ms);
    return appendCommand({"PEXPIREAT", key, std::to_string(expire_at)});
}

bool AppendOnlyFile::appendCommand(const std::vector<std::string>& parts) {
    std::string encoded = encodeCommand(parts);
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        MINIREDIS_LOG_ERROR("aof") << "AOF is not open";
        return false;
    }
    if (!writeAll(encoded)) return false;
    if (rewrite_running_) {
        if (!rewrite_abort_requested_ &&
            rewrite_buffer_.size() + encoded.size() <= rewrite_buffer_limit_bytes_) {
            rewrite_buffer_.append(encoded);
            Stats::instance().setAofRewriteBufferBytes(rewrite_buffer_.size());
        } else if (!rewrite_abort_requested_) {
            rewrite_abort_requested_ = true;
            Stats::instance().setAofRewriteStatus(
                "failed",
                "AOF rewrite buffer limit exceeded; old AOF remains active");
            MINIREDIS_LOG_ERROR("aof") << "AOF rewrite buffer limit exceeded: "
                                       << rewrite_buffer_.size() + encoded.size()
                                       << " > " << rewrite_buffer_limit_bytes_;
        }
    }
    return maybeFsyncLocked();
}

bool AppendOnlyFile::writeAll(const std::string& data) {
    return writeAllToFd(fd_, data);
}

bool AppendOnlyFile::writeAllToFd(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        MINIREDIS_LOG_ERROR("aof") << "failed to write AOF: " << std::strerror(errno);
        return false;
    }
    return true;
}

bool AppendOnlyFile::maybeFsyncLocked() {
    if (policy_ == AppendFsyncPolicy::No) return true;
    if (policy_ == AppendFsyncPolicy::Always) return fsyncLocked();

    auto now = std::chrono::steady_clock::now();
    if (now - last_fsync_ >= std::chrono::seconds(1)) {
        return fsyncLocked();
    }
    return true;
}

bool AppendOnlyFile::fsyncLocked() {
    if (fd_ < 0) return true;
    if (::fsync(fd_) != 0) {
        MINIREDIS_LOG_ERROR("aof") << "fsync failed: " << std::strerror(errno);
        return false;
    }
    last_fsync_ = std::chrono::steady_clock::now();
    return true;
}

uint64_t AppendOnlyFile::currentUnixMillis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

uint64_t AppendOnlyFile::expireAtMillis(int ttl_seconds) {
    return currentUnixMillis() + static_cast<uint64_t>(ttl_seconds) * 1000ULL;
}

std::string AppendOnlyFile::encodeCommand(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& part : parts) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out += part;
        out += "\r\n";
    }
    return out;
}

bool AppendOnlyFile::applyCommand(const std::vector<std::string>& parts, SnapshotData& data) {
    if (parts.empty()) return false;
    std::string cmd = toUpper(parts[0]);
    uint64_t now_ms = currentUnixMillis();

    if (cmd == "SET") {
        if (parts.size() != 3 && parts.size() != 5) return false;
        uint64_t expire_at_ms = 0;
        if (parts.size() == 5) {
            if (toUpper(parts[3]) != "PXAT" || !parseU64(parts[4], expire_at_ms)) {
                return false;
            }
            if (expire_at_ms <= now_ms) {
                data.erase(parts[1]);
                return true;
            }
        }
        data[parts[1]] = SnapshotEntry{parts[2], expire_at_ms};
        return true;
    }

    if (cmd == "DEL") {
        if (parts.size() < 2) return false;
        for (size_t i = 1; i < parts.size(); ++i) {
            data.erase(parts[i]);
        }
        return true;
    }

    if (cmd == "PEXPIREAT") {
        if (parts.size() != 3) return false;
        uint64_t expire_at_ms = 0;
        if (!parseU64(parts[2], expire_at_ms)) return false;
        auto it = data.find(parts[1]);
        if (it == data.end()) return true;
        if (expire_at_ms <= now_ms) {
            data.erase(it);
        } else {
            it->second.expire_at_ms = expire_at_ms;
        }
        return true;
    }

    return false;
}

bool AppendOnlyFile::replay(SnapshotData& data) const {
    std::ifstream ifs(path_, std::ios::binary);
    if (!ifs) return true;

    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxAofBytes) {
        MINIREDIS_LOG_ERROR("aof") << "AOF file too large to replay: " << path_;
        return false;
    }
    ifs.seekg(0, std::ios::beg);

    std::string content(static_cast<size_t>(size), '\0');
    if (!content.empty()) {
        ifs.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ifs) {
            MINIREDIS_LOG_ERROR("aof") << "failed to read AOF: " << path_;
            return false;
        }
    }

    RespDecoder decoder;
    decoder.feed(content);

    size_t applied = 0;
    while (decoder.bufferedSize() > 0) {
        auto value = decoder.parse();
        if (!value) {
            MINIREDIS_LOG_WARN("aof") << "ignore incomplete or invalid AOF tail: " << path_;
            break;
        }
        if (value->type != RespType::ARRAY) {
            MINIREDIS_LOG_ERROR("aof") << "invalid AOF record type";
            return false;
        }

        std::vector<std::string> parts;
        parts.reserve(value->array.size());
        for (const auto& arg : value->array) {
            if (arg.type != RespType::BULK_STRING && arg.type != RespType::SIMPLE_STRING) {
                MINIREDIS_LOG_ERROR("aof") << "invalid AOF command argument";
                return false;
            }
            parts.push_back(arg.str);
        }
        if (!applyCommand(parts, data)) {
            MINIREDIS_LOG_ERROR("aof") << "invalid AOF command";
            return false;
        }
        ++applied;
    }

    MINIREDIS_LOG_INFO("aof") << "replayed " << applied << " AOF records from " << path_;
    return true;
}

bool AppendOnlyFile::rewrite(const SnapshotData& data) {
    joinRewriteThreadIfIdle();

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        MINIREDIS_LOG_ERROR("aof") << "AOF is not open";
        return false;
    }
    if (rewrite_running_) {
        MINIREDIS_LOG_WARN("aof") << "AOF rewrite is already running";
        Stats::instance().setAofRewriteStatus("busy", "AOF rewrite is already running");
        return false;
    }
    rewrite_running_ = true;
    rewrite_abort_requested_ = false;
    rewrite_buffer_.clear();
    Stats::instance().setAofRewriteRunning(true);
    Stats::instance().setAofRewriteBufferBytes(0);

    try {
        rewrite_thread_ = std::thread(&AppendOnlyFile::runRewrite, this, data);
    } catch (const std::exception& e) {
        rewrite_running_ = false;
        rewrite_abort_requested_ = false;
        rewrite_buffer_.clear();
        Stats::instance().setAofRewriteRunning(false);
        Stats::instance().setAofRewriteBufferBytes(0);
        Stats::instance().setAofRewriteStatus("failed", e.what());
        MINIREDIS_LOG_ERROR("aof") << "failed to start AOF rewrite thread: " << e.what();
        return false;
    }
    return true;
}

void AppendOnlyFile::joinRewriteThreadIfIdle() {
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rewrite_running_ && rewrite_thread_.joinable()) {
            finished = std::move(rewrite_thread_);
        }
    }
    if (finished.joinable()) {
        finished.join();
    }
}

void AppendOnlyFile::waitForRewrite() {
    std::thread running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rewrite_thread_.joinable()) {
            running = std::move(rewrite_thread_);
        }
    }
    if (running.joinable()) {
        running.join();
    }
}

bool AppendOnlyFile::cleanupRewriteTempLocked() {
    const std::string tmp_path = path_ + ".rewrite.tmp";
    std::error_code ec;
    if (!std::filesystem::exists(tmp_path, ec)) return true;
    std::filesystem::remove(tmp_path, ec);
    if (ec) {
        Stats::instance().setAofRewriteStatus("failed",
            "failed to remove leftover rewrite temp: " + ec.message());
        MINIREDIS_LOG_WARN("aof") << "failed to remove leftover rewrite temp "
                                  << tmp_path << ": " << ec.message();
        return false;
    }
    Stats::instance().setAofRewriteStatus("cleaned_tmp");
    MINIREDIS_LOG_WARN("aof") << "removed leftover rewrite temp AOF: " << tmp_path;
    return true;
}

void AppendOnlyFile::failRewriteLocked(size_t records, size_t duration_ms,
                                       const std::string& error,
                                       const std::string& tmp_path) {
    if (!tmp_path.empty()) {
        ::unlink(tmp_path.c_str());
    }
    rewrite_running_ = false;
    rewrite_abort_requested_ = false;
    rewrite_buffer_.clear();
    Stats::instance().setAofRewriteRunning(false);
    Stats::instance().setAofRewriteBufferBytes(0);
    Stats::instance().setAofRewriteStatus("failed", error);
    Stats::instance().recordAofRewriteResult(false, records, duration_ms);
}

void AppendOnlyFile::runRewrite(SnapshotData data) {
    auto started = std::chrono::steady_clock::now();
    auto duration_ms = [&started]() -> size_t {
        return static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
    };

    std::filesystem::path target(path_);
    std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::string error = "failed to create AOF directory " + parent.string() +
                                ": " + ec.message();
            MINIREDIS_LOG_ERROR("aof") << error;
            std::lock_guard<std::mutex> lock(mutex_);
            failRewriteLocked(0, duration_ms(), error);
            return;
        }
    }

    const std::string tmp_path = path_ + ".rewrite.tmp";
    int tmp_fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (tmp_fd < 0) {
        std::string error = "failed to open rewrite temp AOF: " + tmp_path +
                            " error=" + std::strerror(errno);
        MINIREDIS_LOG_ERROR("aof") << error;
        std::lock_guard<std::mutex> lock(mutex_);
        failRewriteLocked(0, duration_ms(), error);
        return;
    }

    const uint64_t now_ms = currentUnixMillis();
    size_t records = 0;
    bool ok = true;
    for (const auto& [key, entry] : data) {
        if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now_ms) {
            continue;
        }
        std::vector<std::string> parts{"SET", key, entry.value};
        if (entry.expire_at_ms != 0) {
            parts.push_back("PXAT");
            parts.push_back(std::to_string(entry.expire_at_ms));
        }
        if (!writeAllToFd(tmp_fd, encodeCommand(parts))) {
            ok = false;
            break;
        }
        ++records;
    }

    if (!ok) {
        ::close(tmp_fd);
        std::lock_guard<std::mutex> lock(mutex_);
        failRewriteLocked(records, duration_ms(), "failed to write snapshot records", tmp_path);
        return;
    }

    while (true) {
        std::string buffered_writes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (rewrite_abort_requested_) {
                if (::close(tmp_fd) != 0) {
                    MINIREDIS_LOG_WARN("aof") << "close aborted rewrite temp failed: "
                                              << std::strerror(errno);
                }
                failRewriteLocked(records, duration_ms(),
                                  "AOF rewrite aborted because rewrite buffer limit was exceeded",
                                  tmp_path);
                return;
            }
            if (!rewrite_buffer_.empty()) {
                buffered_writes.swap(rewrite_buffer_);
                Stats::instance().setAofRewriteBufferBytes(0);
            } else {
                if (::fsync(tmp_fd) != 0) {
                    MINIREDIS_LOG_ERROR("aof") << "fsync rewrite temp AOF failed: "
                                               << std::strerror(errno);
                    ok = false;
                }
                if (::close(tmp_fd) != 0) {
                    MINIREDIS_LOG_ERROR("aof") << "close rewrite temp AOF failed: "
                                               << std::strerror(errno);
                    ok = false;
                }
                if (!ok) {
                    failRewriteLocked(records, duration_ms(),
                                      "failed to finish rewrite temp file", tmp_path);
                    return;
                }

                const bool was_open = fd_ >= 0;
                if (was_open) {
                    if (policy_ != AppendFsyncPolicy::No) {
                        fsyncLocked();
                    }
                    ::close(fd_);
                    fd_ = -1;
                }

                if (::rename(tmp_path.c_str(), path_.c_str()) != 0) {
                    std::string error = std::string("rename rewrite AOF failed: ") +
                                        std::strerror(errno);
                    MINIREDIS_LOG_ERROR("aof") << error;
                    if (was_open) {
                        fd_ = ::open(path_.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0644);
                    }
                    failRewriteLocked(records, duration_ms(), error, tmp_path);
                    return;
                }

                if (!parent.empty()) {
                    int dir_fd = ::open(parent.string().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (dir_fd >= 0) {
                        if (::fsync(dir_fd) != 0) {
                            MINIREDIS_LOG_WARN("aof")
                                << "fsync AOF directory failed: " << std::strerror(errno);
                        }
                        ::close(dir_fd);
                    }
                }

                if (was_open) {
                    fd_ = ::open(path_.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0644);
                    if (fd_ < 0) {
                        std::string error = "failed to reopen rewritten AOF: " + path_ +
                                            " error=" + std::strerror(errno);
                        MINIREDIS_LOG_ERROR("aof") << error;
                        failRewriteLocked(records, duration_ms(), error);
                        return;
                    }
                    last_fsync_ = std::chrono::steady_clock::now();
                }

                rewrite_running_ = false;
                rewrite_abort_requested_ = false;
                rewrite_buffer_.clear();
                Stats::instance().setAofRewriteRunning(false);
                Stats::instance().setAofRewriteBufferBytes(0);
                break;
            }
        }

        if (!writeAllToFd(tmp_fd, buffered_writes)) {
            if (::close(tmp_fd) != 0) {
                MINIREDIS_LOG_WARN("aof") << "close failed rewrite temp failed: "
                                          << std::strerror(errno);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            failRewriteLocked(records, duration_ms(),
                              "failed to write buffered commands to rewrite temp file",
                              tmp_path);
            return;
        }
    }

    Stats::instance().recordAofRewriteResult(true, records, duration_ms());
    MINIREDIS_LOG_INFO("aof") << "background rewrite finished with "
                              << records << " snapshot records: " << path_;
}

} // namespace miniredis
