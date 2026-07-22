#include "miniredis/net/resp_parser.hpp"
#include <charconv>
#include <limits>

namespace miniredis {
namespace {

constexpr size_t kMaxHeaderBytes = 1024;
constexpr size_t kMaxNestingDepth = 64;
constexpr long long kMaxBulkStringBytes = 512LL * 1024LL * 1024LL;
constexpr long long kMaxArrayElements = 1'000'000LL;

bool parseIntegerStrict(std::string_view text, long long& value) {
    if (text.empty()) return false;
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc() && ptr == end;
}

} // namespace

// ---------- Decoder ----------

void RespDecoder::feed(std::string_view data) {
    buffer_.append(data.data(), data.size());
}

void RespDecoder::reset() {
    buffer_.clear();
    protocol_error_ = false;
    protocol_error_message_.clear();
}

std::optional<RespValue> RespDecoder::parse() {
    if (buffer_.empty() || protocol_error_) return std::nullopt;
    size_t pos = 0;
    auto result = parseValue(buffer_, pos, 0);
    if (result && pos <= buffer_.size()) {
        // 成功解析一个完整值，移除已解析部分
        buffer_.erase(0, pos);
        return result;
    }
    return std::nullopt;
}

std::optional<size_t> RespDecoder::findLineEnd(const std::string& data, size_t pos) {
    size_t end = data.find("\r\n", pos);
    if (end == std::string::npos) {
        if (data.size() - pos > kMaxHeaderBytes) {
            setProtocolError("RESP header exceeds limit");
        }
        return std::nullopt;
    }
    if (end - pos > kMaxHeaderBytes) {
        setProtocolError("RESP header exceeds limit");
        return std::nullopt;
    }
    return end;
}

void RespDecoder::setProtocolError(std::string message) {
    if (protocol_error_) return;
    protocol_error_ = true;
    protocol_error_message_ = std::move(message);
}

std::optional<RespValue> RespDecoder::parseValue(const std::string& data, size_t& pos,
                                                 size_t depth) {
    if (pos >= data.size()) return std::nullopt;
    if (depth > kMaxNestingDepth) {
        setProtocolError("RESP nesting depth exceeds limit");
        return std::nullopt;
    }
    char c = data[pos];
    switch (c) {
        case '+': return parseSimpleString(data, pos);
        case '-': return parseError(data, pos);
        case ':': return parseInteger(data, pos);
        case '$': return parseBulkString(data, pos);
        case '*': return parseArray(data, pos, depth);
        default:
            setProtocolError("unknown RESP type byte");
            return std::nullopt;
    }
}

std::optional<RespValue> RespDecoder::parseSimpleString(const std::string& data, size_t& pos) {
    auto end_value = findLineEnd(data, pos);
    if (!end_value) return std::nullopt;
    size_t end = *end_value;
    RespValue val;
    val.type = RespType::SIMPLE_STRING;
    val.str = data.substr(pos + 1, end - pos - 1);
    pos = end + 2;
    return val;
}//+OK\r\n

std::optional<RespValue> RespDecoder::parseError(const std::string& data, size_t& pos) {
    auto end_value = findLineEnd(data, pos);
    if (!end_value) return std::nullopt;
    size_t end = *end_value;
    RespValue val;
    val.type = RespType::ERROR;
    val.str = data.substr(pos + 1, end - pos - 1);
    pos = end + 2;
    return val;
}//-ERR xxx\r\n

std::optional<RespValue> RespDecoder::parseInteger(const std::string& data, size_t& pos) {
    auto end_value = findLineEnd(data, pos);
    if (!end_value) return std::nullopt;
    size_t end = *end_value;
    std::string_view numStr(data.data() + pos + 1, end - pos - 1);
    long long n = 0;
    if (!parseIntegerStrict(numStr, n)) {
        setProtocolError("invalid RESP integer");
        return std::nullopt;
    }
    RespValue val;
    val.type = RespType::INTEGER;
    val.integer = n;
    pos = end + 2;
    return val;
}//123\r\n

std::optional<RespValue> RespDecoder::parseBulkString(const std::string& data, size_t& pos) {
    auto len_end_value = findLineEnd(data, pos);
    if (!len_end_value) return std::nullopt;
    size_t lenEnd = *len_end_value;
    long long len = 0;
    std::string_view lenStr(data.data() + pos + 1, lenEnd - pos - 1);
    if (!parseIntegerStrict(lenStr, len)) {
        setProtocolError("invalid RESP bulk length");
        return std::nullopt;
    }
    if (len == -1) {
        RespValue val;
        val.type = RespType::BULK_STRING;
        val.str.clear(); // null
        pos = lenEnd + 2;
        return val;
    }
    if (len < 0 || len > kMaxBulkStringBytes) {
        setProtocolError("RESP bulk length out of range");
        return std::nullopt;
    }
    size_t dataStart = lenEnd + 2;
    const size_t value_size = static_cast<size_t>(len);
    if (value_size > std::numeric_limits<size_t>::max() - dataStart - 2) {
        setProtocolError("RESP bulk length overflow");
        return std::nullopt;
    }
    const size_t frame_end = dataStart + value_size + 2;
    if (data.size() < frame_end) return std::nullopt;
    if (data[dataStart + value_size] != '\r' || data[dataStart + value_size + 1] != '\n') {
        setProtocolError("invalid RESP bulk terminator");
        return std::nullopt;
    }
    RespValue val;
    val.type = RespType::BULK_STRING;
    val.str = data.substr(dataStart, value_size);
    pos = frame_end;
    return val;
}//$4\r\njack\r\n

std::optional<RespValue> RespDecoder::parseArray(const std::string& data, size_t& pos,
                                                 size_t depth) {
    auto len_end_value = findLineEnd(data, pos);
    if (!len_end_value) return std::nullopt;
    size_t lenEnd = *len_end_value;
    long long numElements = 0;
    std::string_view lenStr(data.data() + pos + 1, lenEnd - pos - 1);
    if (!parseIntegerStrict(lenStr, numElements) || numElements < 0 ||
        numElements > kMaxArrayElements) {
        setProtocolError("RESP array length out of range");
        return std::nullopt;
    }
    pos = lenEnd + 2;
    std::vector<RespValue> elements;
    for (long long i = 0; i < numElements; ++i) {
        auto elem = parseValue(data, pos, depth + 1);
        if (!elem) return std::nullopt;
        elements.push_back(std::move(*elem));
    }
    RespValue val;
    val.type = RespType::ARRAY;
    val.array = std::move(elements);
    return val;
}//*2\r\n$3\r\nGET\r\n$4\r\nname\r\n

// ---------- Writer ----------

std::string RespWriter::simpleString(const std::string& s) {
    return "+" + s + "\r\n";
}

std::string RespWriter::error(const std::string& msg) {
    return "-ERR " + msg + "\r\n";
}

std::string RespWriter::integer(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string RespWriter::bulkString(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string RespWriter::nullBulkString() {
    return "$-1\r\n";
}

std::string RespWriter::array(const std::vector<std::string>& items) {
    std::string result = "*" + std::to_string(items.size()) + "\r\n";
    for (const auto& item : items) {
        result += bulkString(item);
    }
    return result;
}

} // namespace miniredis
