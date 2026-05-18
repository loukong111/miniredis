#include "resp_parser.hpp"
#include <cctype>
#include <charconv>
#include <iostream>
#include <sstream>

namespace miniredis {

// ---------- Decoder ----------

void RespDecoder::feed(std::string_view data) {
    buffer_.append(data.data(), data.size());
}

void RespDecoder::reset() {
    buffer_.clear();
}

std::optional<RespValue> RespDecoder::parse() {
    if (buffer_.empty()) return std::nullopt;
    size_t pos = 0;
    auto result = parseValue(buffer_, pos);
    if (result && pos <= buffer_.size()) {
        // 成功解析一个完整值，移除已解析部分
        buffer_.erase(0, pos);
        return result;
    }
    return std::nullopt;
}

std::optional<RespValue> RespDecoder::parseValue(const std::string& data, size_t& pos) {
    if (pos >= data.size()) return std::nullopt;
    char c = data[pos];
    switch (c) {
        case '+': return parseSimpleString(data, pos);
        case '-': return parseError(data, pos);
        case ':': return parseInteger(data, pos);
        case '$': return parseBulkString(data, pos);
        case '*': return parseArray(data, pos);
        default: return std::nullopt;
    }
}

std::optional<RespValue> RespDecoder::parseSimpleString(const std::string& data, size_t& pos) {
    size_t end = data.find("\r\n", pos);
    if (end == std::string::npos) return std::nullopt;
    RespValue val;
    val.type = RespType::SIMPLE_STRING;
    val.str = data.substr(pos + 1, end - pos - 1);
    pos = end + 2;
    return val;
}

std::optional<RespValue> RespDecoder::parseError(const std::string& data, size_t& pos) {
    size_t end = data.find("\r\n", pos);
    if (end == std::string::npos) return std::nullopt;
    RespValue val;
    val.type = RespType::ERROR;
    val.str = data.substr(pos + 1, end - pos - 1);
    pos = end + 2;
    return val;
}

std::optional<RespValue> RespDecoder::parseInteger(const std::string& data, size_t& pos) {
    size_t end = data.find("\r\n", pos);
    if (end == std::string::npos) return std::nullopt;
    std::string_view numStr(data.data() + pos + 1, end - pos - 1);
    long long n = 0;
    auto [ptr, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), n);
    if (ec != std::errc()) return std::nullopt;
    RespValue val;
    val.type = RespType::INTEGER;
    val.integer = n;
    pos = end + 2;
    return val;
}

std::optional<RespValue> RespDecoder::parseBulkString(const std::string& data, size_t& pos) {
    size_t lenEnd = data.find("\r\n", pos);
    if (lenEnd == std::string::npos) return std::nullopt;
    long long len = 0;
    std::string_view lenStr(data.data() + pos + 1, lenEnd - pos - 1);
    auto [ptr, ec] = std::from_chars(lenStr.data(), lenStr.data() + lenStr.size(), len);
    if (ec != std::errc()) return std::nullopt;
    if (len == -1) {
        RespValue val;
        val.type = RespType::BULK_STRING;
        val.str.clear(); // null
        pos = lenEnd + 2;
        return val;
    }
    size_t dataStart = lenEnd + 2;
    if (data.size() < dataStart + len + 2) return std::nullopt;
    RespValue val;
    val.type = RespType::BULK_STRING;
    val.str = data.substr(dataStart, len);
    pos = dataStart + len + 2;
    return val;
}

std::optional<RespValue> RespDecoder::parseArray(const std::string& data, size_t& pos) {
    size_t lenEnd = data.find("\r\n", pos);
    if (lenEnd == std::string::npos) return std::nullopt;
    long long numElements = 0;
    std::string_view lenStr(data.data() + pos + 1, lenEnd - pos - 1);
    auto [ptr, ec] = std::from_chars(lenStr.data(), lenStr.data() + lenStr.size(), numElements);
    if (ec != std::errc() || numElements < 0) return std::nullopt;
    pos = lenEnd + 2;
    std::vector<RespValue> elements;
    for (long long i = 0; i < numElements; ++i) {
        auto elem = parseValue(data, pos);
        if (!elem) return std::nullopt;
        elements.push_back(std::move(*elem));
    }
    RespValue val;
    val.type = RespType::ARRAY;
    val.array = std::move(elements);
    return val;
}

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