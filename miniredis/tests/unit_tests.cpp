#include "miniredis/core/cache_store.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/cluster/consistent_hash.hpp"
#include "miniredis/persistence/file_persistence.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/net/resp_parser.hpp"
#include "miniredis/server/command_handler.hpp"
#include "miniredis/server/config.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>

using namespace miniredis;

static void testRespParser() {
    RespDecoder decoder;
    decoder.feed("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    auto value = decoder.parse();
    assert(value.has_value());
    assert(value->type == RespType::ARRAY);
    assert(value->array.size() == 2);
    assert(value->array[0].str == "GET");
    assert(value->array[1].str == "foo");

    decoder.feed("*3\r\n$3\r\nSET\r\n$1\r\na\r\n$5\r\nhello\r\n");
    value = decoder.parse();
    assert(value.has_value());
    assert(value->array.size() == 3);
    assert(value->array[2].str == "hello");
}

static void testMemoryPoolGrow() {
    FixedMemoryPool pool(1);
    void* first = pool.allocate();
    void* second = pool.allocate();
    assert(first != nullptr);
    assert(second != nullptr);
    assert(pool.used_blocks() == 2);
    pool.deallocate(first);
    pool.deallocate(second);
    assert(pool.used_blocks() == 0);
}

static void testCacheStore() {
    FixedMemoryPool pool(2);
    CacheStore cache(pool);
    cache.set("foo", "bar");
    cache.set("large", std::string(256, 'x'));
    assert(cache.get("foo").value() == "bar");
    assert(cache.get("large").value() == std::string(256, 'x'));
    assert(cache.exists("foo"));
    assert(cache.del("foo"));
    assert(!cache.exists("foo"));
}

static void testFilePersistence() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_unit_" + std::to_string(getpid()) + ".dat");
    FilePersistence persistence(path.string());
    std::unordered_map<std::string, std::string> input;
    input["plain"] = "value";
    input["space key"] = "hello world";
    input["multiline"] = std::string("line1\nline2", 11);
    input["binary"] = std::string("a\0b", 3);

    assert(persistence.saveSnapshot(input));
    std::unordered_map<std::string, std::string> output;
    assert(persistence.loadSnapshot(output));
    assert(output == input);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
}

static RespValue bulk(const std::string& value) {
    RespValue resp;
    resp.type = RespType::BULK_STRING;
    resp.str = value;
    return resp;
}

static RespValue command(std::initializer_list<std::string> parts) {
    RespValue resp;
    resp.type = RespType::ARRAY;
    for (const auto& part : parts) {
        resp.array.push_back(bulk(part));
    }
    return resp;
}

static void testClusterHashSlot() {
    assert(clusterHashSlot("123456789") == 12739);
    assert(clusterHashSlot("foo{bar}1") == clusterHashSlot("x{bar}2"));
    assert(clusterHashKey("foo{}bar") == "foo{}bar");
}

static void testClusterCommands() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ConsistentHash ring(8);
    ring.AddNode("127.0.0.1:6366");
    ring.AddNode("127.0.0.1:6367");
    std::mutex ring_mutex;

    CommandHandler handler(cache, pool, config, true, "127.0.0.1:6366", &ring, &ring_mutex);
    bool authenticated = true;

    std::string keyslot = handler.handle(command({"CLUSTER", "KEYSLOT", "foo{bar}1"}), authenticated);
    assert(keyslot == ":" + std::to_string(clusterHashSlot("foo{bar}1")) + "\r\n");

    std::string nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(nodes.find("127.0.0.1:6366") != std::string::npos);
    assert(nodes.find("127.0.0.1:6367") != std::string::npos);
    assert(nodes.find("myself,master") != std::string::npos);

    std::string info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(info.find("cluster_enabled:1") != std::string::npos);
    assert(info.find("cluster_known_nodes:2") != std::string::npos);
}

int main() {
    testRespParser();
    testMemoryPoolGrow();
    testCacheStore();
    testFilePersistence();
    testClusterHashSlot();
    testClusterCommands();
    std::cout << "unit tests passed" << std::endl;
    return 0;
}
