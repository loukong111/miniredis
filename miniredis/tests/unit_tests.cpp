#include "miniredis/core/cache_store.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/cluster/consistent_hash.hpp"
#include "miniredis/persistence/file_persistence.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/core/thread_pool.hpp"
#include "miniredis/net/resp_parser.hpp"
#include "miniredis/metrics/stats.hpp"
#include "miniredis/server/command_handler.hpp"
#include "miniredis/server/config.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

static void testRespParserHandlesPartialFrame() {
    RespDecoder decoder;
    decoder.feed("*2\r\n$3\r\nGET\r\n$");
    assert(!decoder.parse().has_value());
    assert(decoder.bufferedSize() > 0);

    decoder.feed("3\r\nfoo\r\n");
    auto value = decoder.parse();
    assert(value.has_value());
    assert(value->type == RespType::ARRAY);
    assert(value->array.size() == 2);
    assert(value->array[0].str == "GET");
    assert(value->array[1].str == "foo");
    assert(decoder.bufferedSize() == 0);
}

static void testRespParserHandlesPipelinedFrames() {
    RespDecoder decoder;
    decoder.feed("*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");

    auto first = decoder.parse();
    assert(first.has_value());
    assert(first->type == RespType::ARRAY);
    assert(first->array.size() == 1);
    assert(first->array[0].str == "PING");
    assert(decoder.bufferedSize() > 0);

    auto second = decoder.parse();
    assert(second.has_value());
    assert(second->type == RespType::ARRAY);
    assert(second->array.size() == 2);
    assert(second->array[0].str == "GET");
    assert(second->array[1].str == "foo");
    assert(decoder.bufferedSize() == 0);
    assert(!decoder.parse().has_value());
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

static void testThreadPoolRejectsSubmitAfterStop() {
    DynamicThreadPool pool(1, 2, 1, 1);
    auto future = pool.submit([]() { return 42; });
    assert(future.get() == 42);

    pool.stop();
    bool rejected = false;
    try {
        auto ignored = pool.submit([]() { return 1; });
        (void)ignored;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
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

static void testCacheStoreLazyExpiration() {
    FixedMemoryPool pool(2);
    CacheStore cache(pool);

    cache.set("ttl_get", "v", 1);
    assert(pool.used_blocks() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(!cache.get("ttl_get").has_value());
    assert(pool.used_blocks() == 0);
    assert(cache.key_count() == 0);

    cache.set("ttl_exists", "v", 1);
    assert(pool.used_blocks() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(!cache.exists("ttl_exists"));
    assert(pool.used_blocks() == 0);
    assert(cache.key_count() == 0);
}

static void testCacheStoreTtlCleanup() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);

    cache.set("ttl_cleanup_a", "a", 1);
    cache.set("ttl_cleanup_b", "b", 1);
    cache.set("persistent", "c");
    assert(cache.key_count() == 3);
    assert(pool.used_blocks() == 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    size_t removed = cache.cleanup();
    assert(removed == 2);
    assert(cache.key_count() == 1);
    assert(cache.exists("persistent"));
    assert(!cache.exists("ttl_cleanup_a"));
    assert(!cache.exists("ttl_cleanup_b"));
    assert(pool.used_blocks() == 1);
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

static void testFilePersistenceRejectsHugeCount() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_bad_count_" + std::to_string(getpid()) + ".dat");
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << "MINIREDIS_SNAPSHOT_V1\n";
        uint64_t count = 1'000'001ULL;
        ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
    }

    FilePersistence persistence(path.string());
    std::unordered_map<std::string, std::string> output;
    output["sentinel"] = "keep";
    assert(!persistence.loadSnapshot(output));
    assert(output.size() == 1);
    assert(output["sentinel"] == "keep");

    std::filesystem::remove(path);
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

static void testMovedCountsAsCommand() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ConsistentHash ring(8);
    const std::string current_node = "127.0.0.1:6366";
    const std::string other_node = "127.0.0.1:6367";
    ring.AddNode(current_node);
    ring.AddNode(other_node);
    std::mutex ring_mutex;

    std::string moved_key;
    for (int i = 0; i < 1000; ++i) {
        std::string candidate = "moved-key-" + std::to_string(i);
        std::string slot_key = std::to_string(clusterHashSlot(candidate));
        if (ring.GetNode(slot_key) == other_node) {
            moved_key = candidate;
            break;
        }
    }
    assert(!moved_key.empty());

    CommandHandler handler(cache, pool, config, true, current_node, &ring, &ring_mutex);
    bool authenticated = true;
    size_t before = Stats::instance().totalCommands();
    std::string response = handler.handle(command({"GET", moved_key}), authenticated);
    size_t after = Stats::instance().totalCommands();

    assert(response.find("-MOVED ") == 0);
    assert(response.find(other_node) != std::string::npos);
    assert(after == before + 1);
}

static void testClusterRoutesBySlot() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ConsistentHash ring(8);
    ring.AddNode("127.0.0.1:6366");
    ring.AddNode("127.0.0.1:6367");
    std::mutex ring_mutex;

    const std::string current_node = "127.0.0.1:9999";
    const std::string key_a = "user:{42}:name";
    const std::string key_b = "order:{42}:items";
    uint16_t slot = clusterHashSlot(key_a);
    assert(slot == clusterHashSlot(key_b));
    std::string expected_target = ring.GetNode(std::to_string(slot));
    assert(!expected_target.empty());

    CommandHandler handler(cache, pool, config, true, current_node, &ring, &ring_mutex);
    bool authenticated = true;
    std::string response_a = handler.handle(command({"GET", key_a}), authenticated);
    std::string response_b = handler.handle(command({"GET", key_b}), authenticated);
    std::string expected = "-MOVED " + std::to_string(slot) + " " + expected_target + "\r\n";

    assert(response_a == expected);
    assert(response_b == expected);
}

static void testAuthFlow() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    config.requirepass = "secret";
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);

    bool authenticated = false;
    assert(handler.handle(command({"PING"}), authenticated) ==
           RespWriter::error("NOAUTH Authentication required"));
    assert(!authenticated);

    assert(handler.handle(command({"AUTH", "bad"}), authenticated) ==
           RespWriter::error("invalid password"));
    assert(!authenticated);

    assert(handler.handle(command({"AUTH", "secret"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(authenticated);

    assert(handler.handle(command({"PING"}), authenticated) ==
           RespWriter::simpleString("PONG"));
}

static void testCommandMetadata() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    std::string count_response = handler.handle(command({"COMMAND", "COUNT"}), authenticated);
    assert(count_response == ":8\r\n");

    std::string table_response = handler.handle(command({"COMMAND"}), authenticated);
    RespDecoder decoder;
    decoder.feed(table_response);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 8);

    bool saw_set = false;
    bool saw_cluster = false;
    for (const auto& entry : parsed->array) {
        assert(entry.type == RespType::ARRAY);
        assert(entry.array.size() == 6);
        assert(entry.array[0].type == RespType::BULK_STRING);
        if (entry.array[0].str == "set") {
            saw_set = true;
            assert(entry.array[1].integer == 3);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "cluster") {
            saw_cluster = true;
        }
    }
    assert(saw_set);
    assert(saw_cluster);
}

int main() {
    testRespParser();
    testRespParserHandlesPartialFrame();
    testRespParserHandlesPipelinedFrames();
    testMemoryPoolGrow();
    testThreadPoolRejectsSubmitAfterStop();
    testCacheStore();
    testCacheStoreLazyExpiration();
    testCacheStoreTtlCleanup();
    testFilePersistence();
    testFilePersistenceRejectsHugeCount();
    testClusterHashSlot();
    testClusterCommands();
    testMovedCountsAsCommand();
    testClusterRoutesBySlot();
    testAuthFlow();
    testCommandMetadata();
    std::cout << "unit tests passed" << std::endl;
    return 0;
}
