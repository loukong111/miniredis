#include "miniredis/server/replication_state.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/random.h>
#include <unistd.h>

namespace miniredis {
namespace {

constexpr std::string_view kStateMagic = "MINIREDIS_REPL_STATE_V1";
constexpr size_t kMaxStateFileBytes = 1024;

bool fillRandomBytes(unsigned char* data, size_t size) {
    size_t filled = 0;
    while (filled < size) {
        const ssize_t n = ::getrandom(data + filled, size - filled, 0);
        if (n > 0) {
            filled += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    if (filled == size) return true;

    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (filled < size) {
        const ssize_t n = ::read(fd, data + filled, size - filled);
        if (n > 0) {
            filled += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        ::close(fd);
        return false;
    }
    return ::close(fd) == 0;
}

bool parseOffset(std::string_view text, uint64_t& offset) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, offset);
    return ec == std::errc() && ptr == end;
}

bool writeAll(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool fsyncParentDirectory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path().empty()
                                             ? std::filesystem::path(".")
                                             : path.parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    return ok && closed;
}

} // namespace

bool isValidReplicationId(std::string_view replid) {
    if (replid.size() != kReplicationIdLength) return false;
    for (const char ch : replid) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower_hex = ch >= 'a' && ch <= 'f';
        if (!digit && !lower_hex) return false;
    }
    return true;
}

std::string generateReplicationId() {
    std::array<unsigned char, kReplicationIdLength / 2> bytes{};
    if (!fillRandomBytes(bytes.data(), bytes.size())) {
        throw std::runtime_error("failed to generate replication ID");
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string replid;
    replid.resize(kReplicationIdLength);
    for (size_t i = 0; i < bytes.size(); ++i) {
        replid[i * 2] = kHex[bytes[i] >> 4];
        replid[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    return replid;
}

bool loadReplicationStateFile(const std::string& path, ReplicationState& state) {
    state = ReplicationState{};
    std::error_code ec;
    const uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size == 0 || file_size > kMaxStateFileBytes) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    std::istringstream input(contents);

    std::string first;
    if (!(input >> first)) return false;
    if (first != kStateMagic) {
        uint64_t legacy_offset = 0;
        std::string extra;
        if (!parseOffset(first, legacy_offset) || (input >> extra)) return false;
        state.offset = legacy_offset;
        return true;
    }

    std::string replid_key;
    std::string replid;
    std::string offset_key;
    std::string offset_text;
    std::string extra;
    if (!(input >> replid_key >> replid >> offset_key >> offset_text) ||
        (input >> extra) || replid_key != "master_replid" ||
        offset_key != "offset" || !isValidReplicationId(replid) ||
        !parseOffset(offset_text, state.offset)) {
        state = ReplicationState{};
        return false;
    }
    state.master_replid = std::move(replid);
    return true;
}

bool saveReplicationStateFile(const std::string& path, const ReplicationState& state) {
    if (!isValidReplicationId(state.master_replid)) return false;

    const std::filesystem::path destination(path);
    if (!destination.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) return false;
    }

    const std::string contents = std::string(kStateMagic) + "\nmaster_replid " +
                                 state.master_replid + "\noffset " +
                                 std::to_string(state.offset) + "\n";
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;

    bool ok = writeAll(fd, contents);
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return fsyncParentDirectory(destination);
}

} // namespace miniredis
