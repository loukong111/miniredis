#pragma once

#include "miniredis/core/snapshot.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miniredis {

enum class AppendFsyncPolicy {
    No,
    EverySec,
    Always
};

bool parseAppendFsyncPolicy(const std::string& value, AppendFsyncPolicy& policy);
std::string appendFsyncPolicyName(AppendFsyncPolicy policy);

class AppendOnlyFile {
public:
    AppendOnlyFile(std::string path, AppendFsyncPolicy policy,
                   size_t rewrite_buffer_limit_bytes = 16 * 1024 * 1024);
    ~AppendOnlyFile();

    AppendOnlyFile(const AppendOnlyFile&) = delete;
    AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

    bool open();
    void close();

    bool appendSet(const std::string& key, const std::string& value, int ttl_seconds);
    bool appendDel(const std::vector<std::string>& keys);
    bool appendExpire(const std::string& key, int ttl_seconds);
    bool appendPExpire(const std::string& key, int64_t ttl_ms);

    bool replay(SnapshotData& data) const;
    bool rewrite(const SnapshotData& data);
    const std::string& path() const { return path_; }

private:
    bool appendCommand(const std::vector<std::string>& parts);
    bool writeAll(const std::string& data);
    bool maybeFsyncLocked();
    bool fsyncLocked();
    void joinRewriteThreadIfIdle();
    void waitForRewrite();
    void runRewrite(SnapshotData data);
    bool cleanupRewriteTempLocked();
    void failRewriteLocked(size_t records, size_t duration_ms, const std::string& error,
                           const std::string& tmp_path = {});

    static uint64_t currentUnixMillis();
    static uint64_t expireAtMillis(int ttl_seconds);
    static std::string encodeCommand(const std::vector<std::string>& parts);
    static bool applyCommand(const std::vector<std::string>& parts, SnapshotData& data);
    static bool writeAllToFd(int fd, const std::string& data);

    std::string path_;
    AppendFsyncPolicy policy_;
    size_t rewrite_buffer_limit_bytes_;
    int fd_ = -1;
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point last_fsync_;
    bool rewrite_running_ = false;
    bool rewrite_abort_requested_ = false;
    std::string rewrite_buffer_;
    std::thread rewrite_thread_;
};

} // namespace miniredis
