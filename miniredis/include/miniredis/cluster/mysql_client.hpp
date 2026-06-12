#pragma once

#include <mysql/mysql.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <stdexcept>

namespace miniredis {

class MySQLClient {
public:
    MySQLClient(const std::string& host, const std::string& user,
                const std::string& pass, const std::string& db,
                unsigned int port = 3306);
    ~MySQLClient();

    MySQLClient(const MySQLClient&) = delete;
    MySQLClient& operator=(const MySQLClient&) = delete;

    bool loadAll(std::unordered_map<std::string, std::string>& out);
    bool saveSnapshot(const std::unordered_map<std::string, std::string>& data);
    bool ensureConnection();

    // 在 public 部分添加
    bool registerNode(const std::string& node_addr, int ttl_sec = 30);
    std::vector<std::string> getActiveNodes(int timeout_sec = 30);
    void unregisterNode(const std::string& node_addr);

private:
    void connect();
    void disconnect();
    bool executeQuery(const std::string& sql);

    mutable std::mutex mutex_;
    MYSQL* conn_;
    std::string host_, user_, pass_, db_;
    unsigned int port_;
};

}