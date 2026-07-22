#include "miniredis/core/logger.hpp"
#include "miniredis/cluster/mysql_client.hpp"
#include <stdexcept>
#include <vector>

namespace miniredis {
namespace {

std::string escapeSqlString(MYSQL* connection, const std::string& value) {
    std::vector<char> escaped(value.size() * 2 + 1);
    const unsigned long size = mysql_real_escape_string(
        connection, escaped.data(), value.data(), static_cast<unsigned long>(value.size()));
    return std::string(escaped.data(), size);
}

} // namespace

MySQLClient::MySQLClient(const std::string& host, const std::string& user,
                         const std::string& pass, const std::string& db,
                         unsigned int port)
    : conn_(nullptr), host_(host), user_(user), pass_(pass), db_(db), port_(port) {
    connect();
}

MySQLClient::~MySQLClient() { disconnect(); }

void MySQLClient::connect() {
    if (conn_) disconnect();
    conn_ = mysql_init(nullptr);
    if (!conn_) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    bool reconnect = true;
    mysql_options(conn_, MYSQL_OPT_RECONNECT, &reconnect);

    mysql_options(conn_, MYSQL_DEFAULT_AUTH, "caching_sha2_password");

    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), pass_.c_str(),
                            db_.c_str(), port_, nullptr, 0)) {
        std::string err = "MySQL connect failed: " + std::string(mysql_error(conn_));
        mysql_close(conn_);
        conn_ = nullptr;
        throw std::runtime_error(err);
    }
    mysql_set_character_set(conn_, "utf8mb4");
}

void MySQLClient::disconnect() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

bool MySQLClient::ensureConnection() {
    if (conn_ && mysql_ping(conn_) == 0) return true;
    disconnect();
    try {
        connect();
        return true;
    } catch (...) {
        return false;
    }
}

bool MySQLClient::registerNode(const std::string& node_addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnection()) return false;
    // 使用 INSERT ... ON DUPLICATE KEY UPDATE 更新 last_seen
    const std::string escaped_addr = escapeSqlString(conn_, node_addr);
    std::string sql = "INSERT INTO cluster_nodes (node_addr, last_seen) VALUES ('" + escaped_addr + "', NOW()) "
                      "ON DUPLICATE KEY UPDATE last_seen = NOW()";
    if (mysql_query(conn_, sql.c_str())) {
        MINIREDIS_LOG_ERROR("mysql") << "registerNode failed: " << mysql_error(conn_);
        return false;
    }
    return true;
}

std::vector<std::string> MySQLClient::getActiveNodes(int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> nodes;
    if (!ensureConnection()) return nodes;
    std::string sql = "SELECT node_addr FROM cluster_nodes WHERE last_seen > NOW() - INTERVAL " + std::to_string(timeout_sec) + " SECOND";
    if (mysql_query(conn_, sql.c_str())) {
        MINIREDIS_LOG_ERROR("mysql") << "getActiveNodes query failed: " << mysql_error(conn_);
        return nodes;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return nodes;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0]) nodes.push_back(row[0]);
    }
    mysql_free_result(res);
    return nodes;
}

void MySQLClient::unregisterNode(const std::string& node_addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnection()) return;
    std::string sql = "DELETE FROM cluster_nodes WHERE node_addr = '" +
                      escapeSqlString(conn_, node_addr) + "'";
    mysql_query(conn_, sql.c_str()); 
}

}
