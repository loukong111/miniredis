#include "miniredis/core/cache_store.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/cluster/slot_map.hpp"
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

static void testCacheStoreTtlQuery() {
    FixedMemoryPool pool(2);
    CacheStore cache(pool);

    assert(cache.ttl("missing") == -2);
    cache.set("persistent", "v");
    assert(cache.ttl("persistent") == -1);
    assert(cache.expire("persistent", 2));
    long long ttl = cache.ttl("persistent");
    assert(ttl >= 1 && ttl <= 2);

    cache.set("expiring", "v", 2);
    ttl = cache.ttl("expiring");
    assert(ttl >= 1 && ttl <= 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    assert(cache.ttl("expiring") == -2);
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

static void testClusterSlotMap() {
    ClusterSlotMap slot_map;
    slot_map.Rebuild({"127.0.0.1:6366", "127.0.0.1:6367"});

    assert(slot_map.GetNodeForSlot(0) == "127.0.0.1:6366");
    assert(slot_map.GetNodeForSlot(8191) == "127.0.0.1:6366");
    assert(slot_map.GetNodeForSlot(8192) == "127.0.0.1:6367");
    assert(slot_map.GetNodeForSlot(16383) == "127.0.0.1:6367");
    assert(slot_map.AssignedSlotCount() == kRedisClusterSlots);
    assert(slot_map.GetEpoch() == 1);
    assert(slot_map.FailedNodeCount() == 0);
    slot_map.MarkNodeFailed("127.0.0.1:6367");
    assert(slot_map.IsNodeFailed("127.0.0.1:6367"));
    assert(slot_map.FailedNodeCount() == 1);
    slot_map.MarkNodeHealthy("127.0.0.1:6367");
    assert(!slot_map.IsNodeFailed("127.0.0.1:6367"));

    auto nodes = slot_map.GetAllNodes();
    assert(nodes.size() == 2);
    assert(nodes[0] == "127.0.0.1:6366");
    assert(nodes[1] == "127.0.0.1:6367");

    auto first_ranges = slot_map.GetSlotRangesForNode("127.0.0.1:6366");
    auto second_ranges = slot_map.GetSlotRangesForNode("127.0.0.1:6367");
    assert(first_ranges.size() == 1);
    assert(first_ranges[0].start == 0);
    assert(first_ranges[0].end == 8191);
    assert(second_ranges.size() == 1);
    assert(second_ranges[0].start == 8192);
    assert(second_ranges[0].end == 16383);
}

static void testClusterCommands() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ClusterSlotMap slot_map;
    slot_map.Rebuild({"127.0.0.1:6366", "127.0.0.1:6367"});
    std::mutex slot_map_mutex;

    CommandHandler handler(cache, pool, config, true, "127.0.0.1:6366", &slot_map, &slot_map_mutex);
    bool authenticated = true;

    std::string keyslot = handler.handle(command({"CLUSTER", "KEYSLOT", "foo{bar}1"}), authenticated);
    assert(keyslot == ":" + std::to_string(clusterHashSlot("foo{bar}1")) + "\r\n");

    std::string myid = handler.handle(command({"CLUSTER", "MYID"}), authenticated);
    assert(myid.find(clusterNodeId("127.0.0.1:6366")) != std::string::npos);

    std::string set_resp = handler.handle(command({"SET", "foo{bar}1", "one"}), authenticated);
    assert(set_resp == "+OK\r\n");
    std::string count_resp = handler.handle(command({"CLUSTER", "COUNTKEYSINSLOT", std::to_string(clusterHashSlot("foo{bar}1"))}), authenticated);
    assert(count_resp == ":1\r\n");

    std::string nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(nodes.find("127.0.0.1:6366") != std::string::npos);
    assert(nodes.find("127.0.0.1:6367") != std::string::npos);
    assert(nodes.find("myself,master") != std::string::npos);
    assert(nodes.find("0-8191") != std::string::npos);
    assert(nodes.find("8192-16383") != std::string::npos);

    std::string info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(info.find("cluster_enabled:1") != std::string::npos);
    assert(info.find("cluster_known_nodes:2") != std::string::npos);
    assert(info.find("cluster_failed_nodes:0") != std::string::npos);
    assert(info.find("cluster_slots_assigned:16384") != std::string::npos);
    assert(info.find("cluster_current_epoch:1") != std::string::npos);

    slot_map.MarkNodeFailed("127.0.0.1:6367");
    std::string failed_info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(failed_info.find("cluster_state:fail") != std::string::npos);
    assert(failed_info.find("cluster_failed_nodes:1") != std::string::npos);
    std::string failed_nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(failed_nodes.find("127.0.0.1:6367 master,fail") != std::string::npos);
    assert(failed_nodes.find("disconnected") != std::string::npos);
    slot_map.MarkNodeHealthy("127.0.0.1:6367");

    std::string slots = handler.handle(command({"CLUSTER", "SLOTS"}), authenticated);
    RespDecoder decoder;
    decoder.feed(slots);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 2);
    assert(parsed->array[0].array[0].integer == 0);
    assert(parsed->array[0].array[1].integer == 8191);
    assert(parsed->array[0].array[2].array[0].str == "127.0.0.1");
    assert(parsed->array[0].array[2].array[1].integer == 6366);
}

static void testMovedCountsAsCommand() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ClusterSlotMap slot_map;
    const std::string current_node = "127.0.0.1:6366";
    const std::string other_node = "127.0.0.1:6367";
    slot_map.Rebuild({current_node, other_node});
    std::mutex slot_map_mutex;

    std::string moved_key;
    for (int i = 0; i < 1000; ++i) {
        std::string candidate = "moved-key-" + std::to_string(i);
        if (slot_map.GetNodeForSlot(clusterHashSlot(candidate)) == other_node) {
            moved_key = candidate;
            break;
        }
    }
    assert(!moved_key.empty());

    CommandHandler handler(cache, pool, config, true, current_node, &slot_map, &slot_map_mutex);
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
    ClusterSlotMap slot_map;
    slot_map.Rebuild({"127.0.0.1:6366", "127.0.0.1:6367"});
    std::mutex slot_map_mutex;

    const std::string current_node = "127.0.0.1:9999";
    const std::string key_a = "user:{42}:name";
    const std::string key_b = "order:{42}:items";
    uint16_t slot = clusterHashSlot(key_a);
    assert(slot == clusterHashSlot(key_b));
    std::string expected_target = slot_map.GetNodeForSlot(slot);
    assert(!expected_target.empty());

    CommandHandler handler(cache, pool, config, true, current_node, &slot_map, &slot_map_mutex);
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

static void testCommandSetExAndTtl() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"TTL", "missing"}), authenticated) == ":-2\r\n");
    assert(handler.handle(command({"SET", "session", "token", "EX", "2"}), authenticated) ==
           RespWriter::simpleString("OK"));

    std::string ttl_response = handler.handle(command({"TTL", "session"}), authenticated);
    assert(ttl_response == ":1\r\n" || ttl_response == ":2\r\n");
    assert(handler.handle(command({"GET", "session"}), authenticated) ==
           RespWriter::bulkString("token"));

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    assert(handler.handle(command({"TTL", "session"}), authenticated) == ":-2\r\n");
    assert(handler.handle(command({"GET", "session"}), authenticated) ==
           RespWriter::nullBulkString());

    assert(handler.handle(command({"SET", "bad", "value", "PX", "10"}), authenticated) ==
           RespWriter::error("syntax error"));
}

static void testCommandExpireAndMget() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "k1", "one"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "k2", "two"}), authenticated) ==
           RespWriter::simpleString("OK"));

    std::string mget_response = handler.handle(command({"MGET", "k1", "missing", "k2"}), authenticated);
    RespDecoder decoder;
    decoder.feed(mget_response);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 3);
    assert(parsed->array[0].str == "one");
    assert(parsed->array[1].type == RespType::BULK_STRING);
    assert(parsed->array[1].str.empty());
    assert(parsed->array[2].str == "two");

    assert(handler.handle(command({"EXPIRE", "k1", "2"}), authenticated) == ":1\r\n");
    std::string ttl_response = handler.handle(command({"TTL", "k1"}), authenticated);
    assert(ttl_response == ":1\r\n" || ttl_response == ":2\r\n");
    assert(handler.handle(command({"EXPIRE", "missing", "2"}), authenticated) == ":0\r\n");
    assert(handler.handle(command({"EXPIRE", "k1", "0"}), authenticated) ==
           RespWriter::error("invalid expire time"));
}

static void testCommandMetadata() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    std::string count_response = handler.handle(command({"COMMAND", "COUNT"}), authenticated);
    assert(count_response == ":11\r\n");

    std::string table_response = handler.handle(command({"COMMAND"}), authenticated);
    RespDecoder decoder;
    decoder.feed(table_response);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 11);

    bool saw_set = false;
    bool saw_mget = false;
    bool saw_expire = false;
    bool saw_ttl = false;
    bool saw_cluster = false;
    for (const auto& entry : parsed->array) {
        assert(entry.type == RespType::ARRAY);
        assert(entry.array.size() == 6);
        assert(entry.array[0].type == RespType::BULK_STRING);
        if (entry.array[0].str == "set") {
            saw_set = true;
            assert(entry.array[1].integer == -3);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "ttl") {
            saw_ttl = true;
            assert(entry.array[1].integer == 2);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "mget") {
            saw_mget = true;
            assert(entry.array[1].integer == -2);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == -1);
        }
        if (entry.array[0].str == "expire") {
            saw_expire = true;
            assert(entry.array[1].integer == 3);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "cluster") {
            saw_cluster = true;
        }
    }
    assert(saw_set);
    assert(saw_mget);
    assert(saw_expire);
    assert(saw_ttl);
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
    testCacheStoreTtlQuery();
    testFilePersistence();
    testFilePersistenceRejectsHugeCount();
    testClusterHashSlot();
    testClusterSlotMap();
    testClusterCommands();
    testMovedCountsAsCommand();
    testClusterRoutesBySlot();
    testAuthFlow();
    testCommandSetExAndTtl();
    testCommandExpireAndMget();
    testCommandMetadata();
    std::cout << "unit tests passed" << std::endl;
    return 0;
}
