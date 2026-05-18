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
    RespType type;
    std::string str;                // 用于简单字符串、错误、批量字符串
    long long integer;              // 用于整数
    std::vector<RespValue> array;   // 用于数组
};

// RESP 解码器（增量式，处理半包）
class RespDecoder {
public:
    // 将新数据追加到缓冲区
    void feed(std::string_view data);
    
    // 尝试解析一个完整的 RESP 值
    // 如果解析成功返回值，并移除已解析的数据；否则返回 nullopt
    std::optional<RespValue> parse();

    // 重置解码器（清空缓冲区）
    void reset();

private:
    std::string buffer_;

    // 内部解析函数，从 pos_ 开始尝试解析一个值，返回解析后的长度（包括结尾的 \r\n）
    // 如果解析失败（不完整或错误）返回 -1
    static std::optional<RespValue> parseValue(const std::string& data, size_t& pos);
    static std::optional<RespValue> parseSimpleString(const std::string& data, size_t& pos);
    static std::optional<RespValue> parseError(const std::string& data, size_t& pos);
    static std::optional<RespValue> parseInteger(const std::string& data, size_t& pos);
    static std::optional<RespValue> parseBulkString(const std::string& data, size_t& pos);
    static std::optional<RespValue> parseArray(const std::string& data, size_t& pos);
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