#pragma once

#include <string>
#include <vector>
#include <optional>
#include <string_view>

namespace miniredis {

// RESP 数据类型
enum class RespType {
    SIMPLE_STRING,  // +OK\r\n
    ERROR,          // -ERR\r\n
    INTEGER,        // :100\r\n
    BULK_STRING,    // $6\r\nfoobar\r\n
    ARRAY           // *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
};

// 解析结果：包含类型和值
struct RespValue {
    RespType type = RespType::SIMPLE_STRING;
    std::string str;                // 用于简单字符串、错误、批量字符串
    long long integer = 0;          // 用于整数
    std::vector<RespValue> array;   // 用于数组
};

// RESP 解码器（增量式，处理半包）
class RespDecoder {
public:
    //在这里，feed 会把 string_view 里的内容追加到自己的 buffer_，所以调用方不用构造完整 std::string
    void feed(std::string_view data);

    std::optional<RespValue> parse();

    // 重置解码器（清空缓冲区）
    void reset();
    size_t bufferedSize() const { return buffer_.size(); }
    bool hasProtocolError() const { return protocol_error_; }
    const std::string& protocolError() const { return protocol_error_message_; }

private:
    std::string buffer_;
    bool protocol_error_ = false;
    std::string protocol_error_message_;

    std::optional<size_t> findLineEnd(const std::string& data, size_t pos);
    void setProtocolError(std::string message);
    std::optional<RespValue> parseValue(const std::string& data, size_t& pos, size_t depth);
    std::optional<RespValue> parseSimpleString(const std::string& data, size_t& pos);
    std::optional<RespValue> parseError(const std::string& data, size_t& pos);
    std::optional<RespValue> parseInteger(const std::string& data, size_t& pos);
    std::optional<RespValue> parseBulkString(const std::string& data, size_t& pos);
    std::optional<RespValue> parseArray(const std::string& data, size_t& pos, size_t depth);
};

// RESP 编码器（将响应写入 string）
class RespWriter {
public:
    static std::string simpleString(const std::string& s);   // +OK\r\n
    static std::string error(const std::string& msg);        // -ERR msg\r\n
    static std::string integer(long long n);                 // :100\r\n
    static std::string bulkString(const std::string& s);     // $len\r\ns\r\n
    static std::string nullBulkString();                     // $-1\r\n
    static std::string array(const std::vector<std::string>& items);
};

} // namespace miniredis
