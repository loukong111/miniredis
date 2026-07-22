#pragma once

#include <mysql/mysql.h>
#include <mutex>
#include <string>
#include <vector>

namespace miniredis {

class MySQLClient {
public:
    MySQLClient(const std::string& host, const std::string& user,
                const std::string& pass, const std::string& db,
                unsigned int port = 3306);
    ~MySQLClient();

    MySQLClient(const MySQLClient&) = delete;
    MySQLClient& operator=(const MySQLClient&) = delete;

    bool registerNode(const std::string& node_addr);
    std::vector<std::string> getActiveNodes(int timeout_sec = 30);
    void unregisterNode(const std::string& node_addr);

private:
    void connect();
    void disconnect();
    bool ensureConnection();

    mutable std::mutex mutex_;
    MYSQL* conn_;
    std::string host_, user_, pass_, db_;
    unsigned int port_;
};

}
