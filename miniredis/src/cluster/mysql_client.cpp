#include "miniredis/cluster/mysql_client.hpp"
#include <iostream>
#include <sstream>

namespace miniredis {

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

bool MySQLClient::executeQuery(const std::string& sql) {
    if (!ensureConnection()) return false;
    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "MySQL query error: " << mysql_error(conn_) << "\nSQL: " << sql << std::endl;
        return false;
    }
    return true;
}

bool MySQLClient::loadAll(std::unordered_map<std::string, std::string>& out) {
    if (!ensureConnection()) return false;
    const std::string sql = "SELECT cache_key, cache_value FROM cache_snapshot";
    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "MySQL loadAll error: " << mysql_error(conn_) << std::endl;
        return false;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return false;
    MYSQL_ROW row;
    out.clear();
    while ((row = mysql_fetch_row(res))) {
        if (row[0] && row[1]) out[row[0]] = row[1];
    }
    mysql_free_result(res);
    return true;
}

bool MySQLClient::saveSnapshot(const std::unordered_map<std::string, std::string>& data) {
    if (!ensureConnection()) return false;
    if (!executeQuery("START TRANSACTION")) return false;
    if (!executeQuery("TRUNCATE TABLE cache_snapshot")) {
        executeQuery("ROLLBACK");
        return false;
    }
    if (data.empty()) return executeQuery("COMMIT");
    std::ostringstream oss;
    oss << "INSERT INTO cache_snapshot (cache_key, cache_value) VALUES ";
    bool first = true;
    for (const auto& [key, value] : data) {
        if (!first) oss << ",";
        first = false;
        char* escapedKey = new char[key.length() * 2 + 1];
        char* escapedVal = new char[value.length() * 2 + 1];
        mysql_real_escape_string(conn_, escapedKey, key.c_str(), key.length());
        mysql_real_escape_string(conn_, escapedVal, value.c_str(), value.length());
        oss << "('" << escapedKey << "','" << escapedVal << "')";
        delete[] escapedKey;
        delete[] escapedVal;
    }
    if (mysql_query(conn_, oss.str().c_str())) {
        std::cerr << "MySQL insert error: " << mysql_error(conn_) << std::endl;
        executeQuery("ROLLBACK");
        return false;
    }
    return executeQuery("COMMIT");
}

bool MySQLClient::registerNode(const std::string& node_addr, int ttl_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnection()) return false;
    // 使用 INSERT ... ON DUPLICATE KEY UPDATE 更新 last_seen
    std::string sql = "INSERT INTO cluster_nodes (node_addr, last_seen) VALUES ('" + node_addr + "', NOW()) "
                      "ON DUPLICATE KEY UPDATE last_seen = NOW()";
    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "registerNode failed: " << mysql_error(conn_) << std::endl;
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
        std::cerr << "getActiveNodes query failed: " << mysql_error(conn_) << std::endl;
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
    std::string sql = "DELETE FROM cluster_nodes WHERE node_addr = '" + node_addr + "'";
    mysql_query(conn_, sql.c_str()); 
}

}