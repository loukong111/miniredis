#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace miniredis {

struct ReplicationBacklogEntry {
    uint64_t offset = 0;
    std::vector<std::string> command;
};

class ReplicationBacklog {
public:
    explicit ReplicationBacklog(size_t max_entries = 10000) : max_entries_(max_entries) {}

    uint64_t append(std::vector<std::string> command) {
        std::lock_guard<std::mutex> lock(mutex_);
        ReplicationBacklogEntry entry;
        entry.offset = ++current_offset_;
        entry.command = std::move(command);
        entries_.push_back(std::move(entry));
        while (entries_.size() > max_entries_) {
            entries_.pop_front();
        }
        return current_offset_;
    }

    bool entriesAfter(uint64_t last_offset, std::vector<ReplicationBacklogEntry>& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out.clear();
        if (last_offset > current_offset_) return false;
        if (last_offset == current_offset_) return true;
        if (!entries_.empty() && last_offset < entries_.front().offset - 1) return false;
        for (const auto& entry : entries_) {
            if (entry.offset > last_offset) out.push_back(entry);
        }
        return true;
    }

    uint64_t currentOffset() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_offset_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    size_t max_entries_;
    mutable std::mutex mutex_;
    std::deque<ReplicationBacklogEntry> entries_;
    uint64_t current_offset_ = 0;
};

} // namespace miniredis
