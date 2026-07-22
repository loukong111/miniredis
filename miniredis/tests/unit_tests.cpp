#include "miniredis/core/cache_store.hpp"
#include "miniredis/core/logger.hpp"
#include "miniredis/cluster/cluster_config_store.hpp"
#include "miniredis/cluster/cluster_utils.hpp"
#include "miniredis/cluster/slot_map.hpp"
#include "miniredis/persistence/append_only_file.hpp"
#include "miniredis/persistence/file_persistence.hpp"
#include "miniredis/core/memory_pool.hpp"
#include "miniredis/core/thread_pool.hpp"
#include "miniredis/net/resp_parser.hpp"
#include "miniredis/metrics/stats.hpp"
#include "miniredis/server/command_handler.hpp"
#include "miniredis/server/config.hpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

using namespace miniredis;

static RespValue command(std::initializer_list<std::string> parts);

static uint64_t unixMillisNow() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

static std::string rawRespCommand(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& part : parts) {
        out += "$" + std::to_string(part.size()) + "\r\n";
        out += part;
        out += "\r\n";
    }
    return out;
}

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

static void testRespParserRejectsMalformedFrames() {
    RespDecoder decoder;
    decoder.feed(":12x\r\n");
    assert(!decoder.parse().has_value());
    assert(decoder.hasProtocolError());

    decoder.reset();
    decoder.feed("$3\r\nabcxx");
    assert(!decoder.parse().has_value());
    assert(decoder.hasProtocolError());

    decoder.reset();
    decoder.feed("$-2\r\n");
    assert(!decoder.parse().has_value());
    assert(decoder.hasProtocolError());

    decoder.reset();
    decoder.feed("?invalid\r\n");
    assert(!decoder.parse().has_value());
    assert(decoder.hasProtocolError());

    decoder.reset();
    decoder.feed("$3\r\nab");
    assert(!decoder.parse().has_value());
    assert(!decoder.hasProtocolError());
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

static void testThreadPoolDoesNotShrinkBelowMinimum() {
    DynamicThreadPool pool(2, 4, 1, 1);
    std::atomic<bool> release{false};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 12; ++i) {
        futures.push_back(pool.submit([&release]() {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }));
    }

    const auto expand_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (pool.active_threads() < 4 && std::chrono::steady_clock::now() < expand_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(pool.active_threads() == 4);

    release.store(true, std::memory_order_release);
    for (auto& future : futures) future.get();

    const auto shrink_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (pool.active_threads() > 2 && std::chrono::steady_clock::now() < shrink_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(pool.active_threads() == 2);
    pool.stop();
    assert(pool.active_threads() == 0);
}

static void testLoggerWritesFileAndFiltersLevel() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_log_" + std::to_string(getpid()) + ".log");

    assert(Logger::instance().configure(LogLevel::Warn, path.string()));
    MINIREDIS_LOG_INFO("test") << "filtered info";
    MINIREDIS_LOG_WARN("test") << "visible warn";

    std::ifstream ifs(path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    assert(content.find("visible warn") != std::string::npos);
    assert(content.find("filtered info") == std::string::npos);
    assert(content.find("[WARN] [test]") != std::string::npos);

    assert(Logger::instance().configure(LogLevel::Info, ""));
    std::filesystem::remove(path);
}

static void testConfigFileParsingAndPrecedence() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_config_" + std::to_string(getpid()) + ".conf");
    {
        std::ofstream ofs(path);
        ofs << "# MiniRedis config\n";
        ofs << "log_level = warn\n";
        ofs << "log_file = /tmp/miniredis_from_config.log\n";
        ofs << "bind = 127.0.0.2\n";
        ofs << "port = 18100\n";
        ofs << "stats_port = 18180\n";
        ofs << "snapshot_file = /tmp/miniredis_from_config.dat\n";
        ofs << "snapshot_interval = 33\n";
        ofs << "appendonly_file = /tmp/miniredis_from_config.aof\n";
        ofs << "appendfsync = always\n";
        ofs << "replicaof = 127.0.0.1:7000\n";
        ofs << "replicas = 127.0.0.1:7001,127.0.0.1:7002\n";
        ofs << "replication_backlog_size = 321\n";
        ofs << "replication_sync_interval_ms = 750\n";
        ofs << "replication_reconnect_delay_ms = 250\n";
        ofs << "requirepass = from-file\n";
        ofs << "acl_user = reader:reader-pass:readonly\n";
        ofs << "user = writer password=writer-pass role=readwrite\n";
        ofs << "user = tenant password=tenant-pass role=readwrite commands=get,set,-del keys=tenant:* enabled=true\n";
        ofs << "max_request_bytes = 11111\n";
        ofs << "max_key_bytes = 64\n";
        ofs << "max_value_bytes = 22222\n";
        ofs << "max_pipeline_commands = 7\n";
        ofs << "client_output_buffer_limit = 33333\n";
        ofs << "max_clients = 64\n";
        ofs << "io_threads = 6\n";
        ofs << "cache_shards = 8\n";
        ofs << "maxmemory = 4096\n";
        ofs << "eviction_policy = lru\n";
        ofs << "slowlog_log_slower_than_us = 7\n";
        ofs << "slowlog_max_len = 9\n";
        ofs << "cluster_config_file = /tmp/miniredis_cluster_from_config.conf\n";
        ofs << "cluster = false\n";
    }

    setenv("MINIREDIS_PORT", "18101", 1);
    setenv("MINIREDIS_MAX_CLIENTS", "128", 1);
    setenv("MINIREDIS_MAX_VALUE_BYTES", "44444", 1);

    std::vector<std::string> args = {
        "miniredis",
        "--config", path.string(),
        "--port", "18102",
        "--cluster-config-file", "/tmp/miniredis_cluster_from_cli.conf",
        "--requirepass", "from-cli",
        "--acl-user", "admin:admin-pass:admin",
        "--max-key-bytes", "128",
        "--max-pipeline-commands", "9"
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }

    AppConfig config;
    assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) == ConfigParseResult::Ok);
    assert(config.config_file == path.string());
    assert(config.log_level == "warn");
    assert(config.log_file == "/tmp/miniredis_from_config.log");
    assert(config.bind_addr == "127.0.0.2");
    assert(config.port == 18102);
    assert(config.stats_port == 18180);
    assert(config.snapshot_file == "/tmp/miniredis_from_config.dat");
    assert(config.snapshot_interval_sec == 33);
    assert(config.appendonly_file == "/tmp/miniredis_from_config.aof");
    assert(config.appendfsync == "always");
    assert(config.replicaof == "127.0.0.1:7000");
    assert(config.replicas_str == "127.0.0.1:7001,127.0.0.1:7002");
    assert(config.replication_backlog_size == 321);
    assert(config.replication_sync_interval_ms == 750);
    assert(config.replication_reconnect_delay_ms == 250);
    assert(config.requirepass == "from-cli");
    assert(config.acl_users.size() == 4);
    assert(config.acl_users[0].username == "reader");
    assert(config.acl_users[0].role == AclRole::ReadOnly);
    assert(config.acl_users[1].username == "writer");
    assert(config.acl_users[1].role == AclRole::ReadWrite);
    assert(config.acl_users[2].username == "tenant");
    assert(config.acl_users[2].role == AclRole::ReadWrite);
    assert(config.acl_users[2].enabled);
    assert(config.acl_users[2].command_allowlist_enabled);
    assert(config.acl_users[2].allowed_commands.size() == 2);
    assert(config.acl_users[2].allowed_commands[0] == "GET");
    assert(config.acl_users[2].allowed_commands[1] == "SET");
    assert(config.acl_users[2].denied_commands.size() == 1);
    assert(config.acl_users[2].denied_commands[0] == "DEL");
    assert(!config.acl_users[2].all_keys);
    assert(config.acl_users[2].key_prefixes.size() == 1);
    assert(config.acl_users[2].key_prefixes[0] == "tenant:");
    assert(config.acl_users[3].username == "admin");
    assert(config.acl_users[3].role == AclRole::Admin);
    assert(config.max_request_bytes == 11111);
    assert(config.max_key_bytes == 128);
    assert(config.max_value_bytes == 44444);
    assert(config.max_pipeline_commands == 9);
    assert(config.client_output_buffer_limit == 33333);
    assert(config.max_clients == 128);
    assert(config.io_threads == 6);
    assert(config.cache_shards == 8);
    assert(config.maxmemory_bytes == 4096);
    assert(config.eviction_policy == "lru");
    assert(config.slowlog_log_slower_than_us == 7);
    assert(config.slowlog_max_len == 9);
    assert(config.cluster_config_file == "/tmp/miniredis_cluster_from_cli.conf");
    assert(!config.cluster_mode);

    unsetenv("MINIREDIS_PORT");
    unsetenv("MINIREDIS_MAX_CLIENTS");
    unsetenv("MINIREDIS_MAX_VALUE_BYTES");
    std::filesystem::remove(path);
}

static void testConfigRejectsRemoteBindWithoutAuth() {
    unsetenv("MINIREDIS_REQUIREPASS");
    unsetenv("MINIREDIS_ACL_USERS");

    {
        std::vector<std::string> args = {
            "miniredis",
            "--bind", "0.0.0.0"
        };
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }

        AppConfig config;
        assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) == ConfigParseResult::Error);
    }

    {
        std::vector<std::string> args = {
            "miniredis",
            "--bind", "0.0.0.0",
            "--requirepass", "secret"
        };
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }

        AppConfig config;
        assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) == ConfigParseResult::Ok);
    }
}

static void testConfigRejectsMalformedNumbers() {
    {
        std::vector<std::string> args = {"miniredis", "--port", "6366junk"};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        AppConfig config;
        assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) ==
               ConfigParseResult::Error);
    }
    {
        std::vector<std::string> args = {"miniredis", "--maxmemory", "-1"};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        AppConfig config;
        assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) ==
               ConfigParseResult::Error);
    }

    setenv("MINIREDIS_MAX_CLIENTS", "100oops", 1);
    std::vector<std::string> args = {"miniredis"};
    std::vector<char*> argv{args[0].data()};
    AppConfig config;
    assert(parseConfig(static_cast<int>(argv.size()), argv.data(), config) ==
           ConfigParseResult::Error);
    unsetenv("MINIREDIS_MAX_CLIENTS");

    int port = 0;
    assert(!parseNodePort("127.0.0.1:6366junk", port));
    assert(!parseNodePort("127.0.0.1:-1", port));
    assert(parseNodePort("127.0.0.1:6366", port));
    assert(port == 6366);
}

static void testCacheStore() {
    FixedMemoryPool pool(2);
    CacheStore cache(pool);
    assert(cache.set("foo", "bar") == SetResult::Ok);
    assert(cache.set("large", std::string(256, 'x')) == SetResult::Ok);
    assert(cache.get("foo").value() == "bar");
    assert(cache.get("large").value() == std::string(256, 'x'));
    assert(cache.exists("foo"));
    assert(cache.del("foo"));
    assert(!cache.exists("foo"));
}

static void testCacheStoreShards() {
    FixedMemoryPool pool(16);
    CacheStore cache(pool, 4);
    assert(cache.shard_count() == 4);

    for (int i = 0; i < 12; ++i) {
        assert(cache.set("key" + std::to_string(i), "v" + std::to_string(i)) == SetResult::Ok);
    }
    assert(cache.key_count() == 12);
    assert(cache.get("key7").value() == "v7");
    assert(cache.del("key7"));
    assert(!cache.get("key7").has_value());
    assert(cache.key_count() == 11);

    SnapshotData snapshot = cache.snapshot();
    assert(snapshot.size() == 11);

    FixedMemoryPool restored_pool(16);
    CacheStore restored(restored_pool, 4);
    restored.load_snapshot(snapshot);
    assert(restored.shard_count() == 4);
    assert(restored.key_count() == 11);
    assert(restored.get("key1").value() == "v1");
}

static void testCacheStoreMaxmemoryNoEviction() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    cache.configure_memory_limit(8, EvictionPolicy::NoEviction);

    assert(cache.set("a", "123") == SetResult::Ok);
    assert(cache.used_memory_bytes() == 4);
    assert(cache.set("b", "12345") == SetResult::OutOfMemory);
    assert(!cache.exists("b"));
    assert(cache.evicted_keys() == 0);
}

static void testCacheStoreMaxmemoryLru() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    cache.configure_memory_limit(8, EvictionPolicy::Lru);

    assert(cache.set("a", "111") == SetResult::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(cache.set("b", "222") == SetResult::Ok);
    assert(cache.get("a").value() == "111");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(cache.set("c", "333") == SetResult::Ok);

    assert(cache.exists("a"));
    assert(!cache.exists("b"));
    assert(cache.exists("c"));
    assert(cache.evicted_keys() == 1);
    assert(cache.used_memory_bytes() <= cache.max_memory_bytes());
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
    SnapshotData input;
    input["plain"] = SnapshotEntry{"value", 0};
    input["space key"] = SnapshotEntry{"hello world", 0};
    input["multiline"] = SnapshotEntry{std::string("line1\nline2", 11), 0};
    input["binary"] = SnapshotEntry{std::string("a\0b", 3), unixMillisNow() + 60'000};

    assert(persistence.saveSnapshot(input));
    SnapshotData output;
    assert(persistence.loadSnapshot(output));
    assert(output == input);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
}

static void testFilePersistenceLoadsV1Snapshot() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_v1_" + std::to_string(getpid()) + ".dat");
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << "MINIREDIS_SNAPSHOT_V1\n";
        uint64_t count = 1;
        uint64_t key_size = 6;
        uint64_t value_size = 5;
        ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
        ofs.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
        ofs.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
        ofs.write("legacy", 6);
        ofs.write("value", 5);
    }

    FilePersistence persistence(path.string());
    SnapshotData output;
    assert(persistence.loadSnapshot(output));
    assert(output.size() == 1);
    assert(output["legacy"].value == "value");
    assert(output["legacy"].expire_at_ms == 0);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".bad");
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
    SnapshotData output;
    output["sentinel"] = SnapshotEntry{"keep", 0};
    assert(!persistence.loadSnapshot(output));
    assert(output.size() == 1);
    assert(output["sentinel"].value == "keep");

    std::filesystem::remove(path);
}

static void testFilePersistenceBackupRecovery() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_recover_" + std::to_string(getpid()) + ".dat");
    FilePersistence persistence(path.string());

    SnapshotData first;
    first["stable"] = SnapshotEntry{"one", 0};
    assert(persistence.saveSnapshot(first));

    SnapshotData second;
    second["stable"] = SnapshotEntry{"two", 0};
    assert(persistence.saveSnapshot(second));
    assert(std::filesystem::exists(path.string() + ".bak"));

    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << "MINIREDIS_SNAPSHOT_V1\n";
        uint64_t count = 1'000'001ULL;
        ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
    }

    SnapshotData recovered;
    assert(persistence.loadSnapshot(recovered));
    assert(recovered.size() == 1);
    assert(recovered["stable"].value == "one");
    assert(std::filesystem::exists(path.string() + ".bad"));

    {
        std::ofstream empty(path, std::ios::binary | std::ios::trunc);
    }
    recovered.clear();
    assert(persistence.loadSnapshot(recovered));
    assert(recovered.size() == 1);
    assert(recovered["stable"].value == "one");

    std::filesystem::remove(path);
    recovered.clear();
    assert(persistence.loadSnapshot(recovered));
    assert(recovered.size() == 1);
    assert(recovered["stable"].value == "one");

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".bak");
    std::filesystem::remove(path.string() + ".bad");
    std::filesystem::remove(path.string() + ".tmp");
}

static void testAppendOnlyFileReplay() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_aof_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);

    {
        AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
        assert(aof.open());
        assert(aof.appendSet("plain", "one", 0));
        assert(aof.appendSet("binary", std::string("a\0b", 3), 0));
        assert(aof.appendSet("ttl", "live", 30));
        assert(aof.appendExpire("plain", 30));
        assert(aof.appendDel({"binary"}));
        aof.close();
    }

    SnapshotData data;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(data));
    assert(data.size() == 2);
    assert(data["plain"].value == "one");
    assert(data["plain"].expire_at_ms > unixMillisNow());
    assert(data["ttl"].value == "live");
    assert(data.find("binary") == data.end());

    std::filesystem::remove(path);
}

static void testAppendOnlyFileIgnoresIncompleteTail() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_aof_bad_tail_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);

    {
        AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
        assert(aof.open());
        assert(aof.appendSet("survives", "ok", 0));
        aof.close();
    }
    const auto valid_size = std::filesystem::file_size(path);

    {
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        ofs << "*3\r\n$3\r\nSET\r\n$6\r\nbroken\r\n$";
    }

    SnapshotData data;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(data));
    assert(data.size() == 1);
    assert(data["survives"].value == "ok");
    assert(data.find("broken") == data.end());
    assert(std::filesystem::file_size(path) == valid_size);

    {
        AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
        assert(aof.open());
        assert(aof.appendSet("after-repair", "visible", 0));
        aof.close();
    }

    SnapshotData after_restart;
    assert(reader.replay(after_restart));
    assert(after_restart.size() == 2);
    assert(after_restart["survives"].value == "ok");
    assert(after_restart["after-repair"].value == "visible");

    std::filesystem::remove(path);
}

static void testAppendOnlyFileIgnoresInterruptedRewriteTemp() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_aof_interrupted_rewrite_" + std::to_string(getpid()) + ".aof");
    const std::string tmp_path = path.string() + ".rewrite.tmp";
    std::filesystem::remove(path);
    std::filesystem::remove(tmp_path);

    {
        AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
        assert(aof.open());
        assert(aof.appendSet("stable", "old", 0));
        aof.close();
    }

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        ofs << rawRespCommand({"SET", "stable", "new-from-temp"});
        ofs << rawRespCommand({"SET", "temp-only", "should-not-load"});
        ofs << "*3\r\n$3\r\nSET\r\n$7\r\npartial";
    }

    SnapshotData before_restart;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(before_restart));
    assert(before_restart.size() == 1);
    assert(before_restart["stable"].value == "old");
    assert(before_restart.find("temp-only") == before_restart.end());

    {
        AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
        assert(aof.open());
        assert(!std::filesystem::exists(tmp_path));
        assert(aof.appendSet("after_restart", "ok", 0));
        aof.close();
    }

    SnapshotData after_restart;
    AppendOnlyFile second_reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(second_reader.replay(after_restart));
    assert(after_restart.size() == 2);
    assert(after_restart["stable"].value == "old");
    assert(after_restart["after_restart"].value == "ok");
    assert(after_restart.find("temp-only") == after_restart.end());

    std::filesystem::remove(path);
    std::filesystem::remove(tmp_path);
}

static void testAppendOnlyFileRewriteBufferLimitAllowsRetry() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_rewrite_limit_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");

    AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always, 1);
    assert(aof.open());
    assert(aof.appendSet("stable", "old", 0));

    SnapshotData large;
    for (int i = 0; i < 20000; ++i) {
        large["key:" + std::to_string(i)] = SnapshotEntry{std::string(128, 'x'), 0};
    }
    large["stable"] = SnapshotEntry{"new", 0};

    assert(aof.rewrite(large));
    assert(aof.appendSet("after_limit", "ok", 0));
    aof.close();

    SnapshotData aborted_data;
    AppendOnlyFile aborted_reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(aborted_reader.replay(aborted_data));
    assert(aborted_data["stable"].value == "old");
    assert(aborted_data["after_limit"].value == "ok");
    assert(aborted_data.find("key:1") == aborted_data.end());
    assert(!std::filesystem::exists(path.string() + ".rewrite.tmp"));

    AppendOnlyFile retry(path.string(), AppendFsyncPolicy::Always, 1);
    assert(retry.open());
    SnapshotData compacted;
    compacted["stable"] = SnapshotEntry{"new", 0};
    compacted["after_limit"] = SnapshotEntry{"ok", 0};
    assert(retry.rewrite(compacted));
    retry.close();

    SnapshotData retried_data;
    AppendOnlyFile retried_reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(retried_reader.replay(retried_data));
    assert(retried_data.size() == 2);
    assert(retried_data["stable"].value == "new");
    assert(retried_data["after_limit"].value == "ok");

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");
}

static void testAppendOnlyFileRewrite() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_rewrite_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");

    AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
    assert(aof.open());
    assert(aof.appendSet("stable", "old", 0));
    assert(aof.appendSet("deleted_key", "gone", 0));
    assert(aof.appendDel({"deleted_key"}));

    SnapshotData compacted;
    compacted["stable"] = SnapshotEntry{"new", 0};
    compacted["ttl"] = SnapshotEntry{"live", unixMillisNow() + 60'000};
    assert(aof.rewrite(compacted));
    assert(aof.appendSet("after_rewrite", "ok", 0));
    aof.close();

    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    assert(content.find("deleted_key") == std::string::npos);
    assert(content.find("old") == std::string::npos);

    SnapshotData data;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(data));
    assert(data.size() == 3);
    assert(data["stable"].value == "new");
    assert(data["ttl"].value == "live");
    assert(data["ttl"].expire_at_ms > unixMillisNow());
    assert(data["after_rewrite"].value == "ok");
    assert(data.find("deleted_key") == data.end());

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");
}

static void testCommandHandlerWritesAof() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_cmd_aof_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);

    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    AppConfig config;
    AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
    assert(aof.open());
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr, &aof);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "a", "one"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "ttl", "live", "EX", "30"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"EXPIRE", "a", "30"}), authenticated) == ":1\r\n");
    assert(handler.handle(command({"DEL", "ttl"}), authenticated) == ":1\r\n");
    aof.close();

    SnapshotData data;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(data));
    assert(data.size() == 1);
    assert(data["a"].value == "one");
    assert(data["a"].expire_at_ms > unixMillisNow());
    assert(data.find("ttl") == data.end());

    std::filesystem::remove(path);
}

static void testCommandHandlerRewriteAof() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_cmd_rewrite_" + std::to_string(getpid()) + ".aof");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");

    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    AppConfig config;
    AppendOnlyFile aof(path.string(), AppendFsyncPolicy::Always);
    assert(aof.open());
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr, &aof);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "a", "one"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "a", "two"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "gone", "value"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"DEL", "gone"}), authenticated) == ":1\r\n");
    assert(handler.handle(command({"BGREWRITEAOF"}), authenticated) ==
           RespWriter::simpleString("Background append only file rewriting started"));
    aof.close();

    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    assert(content.find("gone") == std::string::npos);
    assert(content.find("one") == std::string::npos);

    SnapshotData data;
    AppendOnlyFile reader(path.string(), AppendFsyncPolicy::EverySec);
    assert(reader.replay(data));
    assert(data.size() == 1);
    assert(data["a"].value == "two");

    CommandHandler no_aof_handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr, nullptr);
    assert(no_aof_handler.handle(command({"BGREWRITEAOF"}), authenticated) ==
           RespWriter::error("AOF is not enabled"));

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".rewrite.tmp");
}

static void testCacheStoreSnapshotRestoresTtl() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    assert(cache.set("persistent", "value") == SetResult::Ok);
    assert(cache.set("session", "token", 3) == SetResult::Ok);

    SnapshotData snapshot = cache.snapshot();
    assert(snapshot.size() == 2);
    assert(snapshot["persistent"].value == "value");
    assert(snapshot["persistent"].expire_at_ms == 0);
    assert(snapshot["session"].value == "token");
    assert(snapshot["session"].expire_at_ms > unixMillisNow());

    FixedMemoryPool restored_pool(4);
    CacheStore restored(restored_pool);
    restored.load_snapshot(snapshot);
    assert(restored.get("persistent").value() == "value");
    assert(restored.get("session").value() == "token");
    long long ttl = restored.ttl("session");
    assert(ttl >= 1 && ttl <= 3);
}

static void testCacheStoreSnapshotSkipsExpiredEntries() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    SnapshotData snapshot;
    snapshot["old"] = SnapshotEntry{"value", unixMillisNow() - 1000};
    snapshot["live"] = SnapshotEntry{"value", unixMillisNow() + 60'000};

    cache.load_snapshot(snapshot);
    assert(!cache.exists("old"));
    assert(cache.exists("live"));
    assert(cache.key_count() == 1);
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
    assert(slot_map.SuspectNodeCount() == 0);
    slot_map.MarkNodeSuspect("127.0.0.1:6367");
    assert(slot_map.GetEpoch() == 2);
    assert(slot_map.GetNodeState("127.0.0.1:6367") == ClusterNodeState::Suspect);
    assert(slot_map.SuspectNodeCount() == 1);
    slot_map.MarkNodeFailed("127.0.0.1:6367");
    assert(slot_map.GetEpoch() == 3);
    assert(slot_map.IsNodeFailed("127.0.0.1:6367"));
    assert(slot_map.FailedNodeCount() == 1);
    slot_map.MarkNodeHealthy("127.0.0.1:6367");
    assert(slot_map.GetEpoch() == 4);
    assert(!slot_map.IsNodeFailed("127.0.0.1:6367"));
    slot_map.MarkNodeHealthy("127.0.0.1:6367");
    assert(slot_map.GetEpoch() == 4);

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
    assert(slot_map.SetSlotState(0, ClusterSlotState::Migrating, "127.0.0.1:6367"));
    ClusterSlotMeta meta = slot_map.GetSlotMeta(0);
    assert(meta.state == ClusterSlotState::Migrating);
    assert(meta.peer_node == "127.0.0.1:6367");
    assert(slot_map.SetSlotOwner(0, "127.0.0.1:6367"));
    assert(slot_map.GetNodeForSlot(0) == "127.0.0.1:6367");
    assert(slot_map.GetSlotMeta(0).state == ClusterSlotState::Stable);
    assert(slot_map.AddNode("127.0.0.1:6368"));
    assert(slot_map.GetEpoch() == 7);
    assert(!slot_map.NodeOwnsSlots("127.0.0.1:6368"));
    assert(slot_map.RemoveNode("127.0.0.1:6368"));
    assert(slot_map.GetEpoch() == 8);
    assert(slot_map.NodeOwnsSlots("127.0.0.1:6367"));
    assert(!slot_map.RemoveNode("127.0.0.1:6367"));

    ClusterSlotMapSnapshot snapshot = slot_map.ExportSnapshot();
    ClusterSlotMap restored;
    assert(restored.LoadSnapshot(snapshot));
    assert(restored.GetNodeForSlot(0) == "127.0.0.1:6367");
    assert(restored.GetNodeForSlot(16383) == "127.0.0.1:6367");
    assert(restored.GetEpoch() == slot_map.GetEpoch());
    ClusterSlotMapSnapshot stale = snapshot;
    stale.epoch = snapshot.epoch - 1;
    assert(!restored.LoadSnapshotIfNewer(stale));
    ClusterSlotMapSnapshot newer = snapshot;
    newer.epoch = snapshot.epoch + 1;
    assert(newer.slot_owner[1] == "127.0.0.1:6366");
    newer.slot_owner[1] = "127.0.0.1:6367";
    assert(restored.LoadSnapshotIfNewer(newer));
    assert(restored.GetEpoch() == newer.epoch);
    assert(restored.GetNodeForSlot(1) == "127.0.0.1:6367");
}

static void testClusterConfigStore() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("miniredis_cluster_" + std::to_string(getpid()) + ".conf");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");

    ClusterSlotMap slot_map;
    slot_map.Rebuild({"127.0.0.1:6366", "127.0.0.1:6367"});
    slot_map.MarkNodeSuspect("127.0.0.1:6367");
    assert(slot_map.SetSlotState(0, ClusterSlotState::Migrating, "127.0.0.1:6367"));
    assert(saveClusterConfig(path.string(), slot_map));

    ClusterSlotMap loaded;
    assert(loadClusterConfig(path.string(), loaded));
    assert(loaded.GetNodeForSlot(0) == "127.0.0.1:6366");
    assert(loaded.GetNodeForSlot(8191) == "127.0.0.1:6366");
    assert(loaded.GetNodeForSlot(8192) == "127.0.0.1:6367");
    assert(loaded.GetNodeForSlot(16383) == "127.0.0.1:6367");
    assert(loaded.GetNodeState("127.0.0.1:6367") == ClusterNodeState::Suspect);
    assert(loaded.SuspectNodeCount() == 1);
    ClusterSlotMeta meta = loaded.GetSlotMeta(0);
    assert(meta.state == ClusterSlotState::Migrating);
    assert(meta.peer_node == "127.0.0.1:6367");
    assert(loaded.GetEpoch() == slot_map.GetEpoch());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "MINIREDIS_CLUSTER_CONFIG_V1\n"
            << "epoch 9\n"
            << "nodes 2\n"
            << "node 127.0.0.1:6366 healthy\n"
            << "node 127.0.0.1:6367 healthy\n"
            << "slots 2\n"
            << "slot 0 10 127.0.0.1:6366\n"
            << "slot 10 20 127.0.0.1:6367\n"
            << "slotstates 0\n";
    }
    ClusterSlotMap invalid;
    assert(!loadClusterConfig(path.string(), invalid));

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
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
    assert(info.find("cluster_suspect_nodes:0") != std::string::npos);
    assert(info.find("cluster_slots_assigned:16384") != std::string::npos);
    assert(info.find("cluster_current_epoch:1") != std::string::npos);

    std::string slotmap = handler.handle(command({"CLUSTER", "SLOTMAP"}), authenticated);
    RespDecoder slotmap_decoder;
    slotmap_decoder.feed(slotmap);
    auto slotmap_value = slotmap_decoder.parse();
    assert(slotmap_value.has_value());
    assert(slotmap_value->type == RespType::ARRAY);
    assert(slotmap_value->array.size() == 4);
    assert(slotmap_value->array[0].array[0].str == "epoch");
    assert(slotmap_value->array[0].array[1].integer == 1);
    assert(slotmap_value->array[1].array[0].str == "nodes");
    assert(slotmap_value->array[1].array[1].type == RespType::ARRAY);
    assert(slotmap_value->array[2].array[0].str == "slots");
    assert(slotmap_value->array[2].array[1].array.size() == 2);
    assert(slotmap_value->array[3].array[0].str == "slotstates");

    assert(handler.handle(command({"CLUSTER", "SETSLOT", "1", "MIGRATING", "127.0.0.1:6367"}), authenticated) ==
           RespWriter::simpleString("OK"));
    ClusterSlotMeta migrating_meta = slot_map.GetSlotMeta(1);
    assert(migrating_meta.state == ClusterSlotState::Migrating);
    assert(migrating_meta.peer_node == "127.0.0.1:6367");
    std::string migrating_nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(migrating_nodes.find("[1->-" + clusterNodeId("127.0.0.1:6367") + "]") != std::string::npos);
    assert(handler.handle(command({"CLUSTER", "SETSLOT", "1", "STABLE"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(slot_map.GetSlotMeta(1).state == ClusterSlotState::Stable);
    assert(handler.handle(command({"CLUSTER", "SETSLOT", "1", "NODE", "127.0.0.1:6367"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(slot_map.GetNodeForSlot(1) == "127.0.0.1:6367");
    assert(handler.handle(command({"CLUSTER", "SETSLOT", "1", "NODE", "127.0.0.1:6366"}), authenticated) ==
           RespWriter::simpleString("OK"));

    slot_map.MarkNodeSuspect("127.0.0.1:6367");
    std::string suspect_info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(suspect_info.find("cluster_state:ok") != std::string::npos);
    assert(suspect_info.find("cluster_suspect_nodes:1") != std::string::npos);
    std::string suspect_nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(suspect_nodes.find("127.0.0.1:6367 master,pfail") != std::string::npos);

    slot_map.MarkNodeFailed("127.0.0.1:6367");
    std::string failed_info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(failed_info.find("cluster_state:fail") != std::string::npos);
    assert(failed_info.find("cluster_failed_nodes:1") != std::string::npos);
    assert(failed_info.find("cluster_suspect_nodes:0") != std::string::npos);
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

    assert(handler.handle(command({"CLUSTER", "MEET", "127.0.0.1:6368"}), authenticated) ==
           RespWriter::simpleString("OK"));
    std::string meet_info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(meet_info.find("cluster_known_nodes:3") != std::string::npos);
    std::string meet_nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(meet_nodes.find("127.0.0.1:6368") != std::string::npos);
    assert(handler.handle(command({"CLUSTER", "FORGET", "127.0.0.1:6367"}), authenticated) ==
           RespWriter::error("node still owns slots; migrate or reassign slots first"));
    assert(handler.handle(command({"CLUSTER", "FORGET", "127.0.0.1:6368"}), authenticated) ==
           RespWriter::simpleString("OK"));
    std::string forget_info = handler.handle(command({"CLUSTER", "INFO"}), authenticated);
    assert(forget_info.find("cluster_known_nodes:2") != std::string::npos);
    assert(handler.handle(command({"CLUSTER", "FORGET", "127.0.0.1:6366"}), authenticated) ==
           RespWriter::error("cannot forget myself"));
    assert(handler.handle(command({"CLUSTER", "MEET", "not-a-node"}), authenticated) ==
           RespWriter::error("invalid node address"));
}

static void testClusterManualFailoverTakeover() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    AppConfig config;
    config.replicaof = "127.0.0.1:6366";
    ClusterSlotMap slot_map;
    const std::string master_node = "127.0.0.1:6366";
    const std::string replica_node = "127.0.0.1:6367";
    slot_map.Rebuild({master_node, replica_node});
    std::mutex slot_map_mutex;
    bool cluster_changed = false;

    CommandHandler handler(cache, pool, config, true, replica_node,
                           &slot_map, &slot_map_mutex, nullptr,
                           [&cluster_changed]() { cluster_changed = true; });
    bool authenticated = true;

    assert(handler.handle(command({"SET", "before-failover", "blocked"}), authenticated) ==
           RespWriter::error("READONLY You can't write against a read only replica"));

    std::string before_repl_info = handler.handle(command({"INFO", "replication"}), authenticated);
    assert(before_repl_info.find("role:slave") != std::string::npos);
    assert(before_repl_info.find("master_node:" + master_node) != std::string::npos);

    assert(handler.handle(command({"CLUSTER", "FAILOVER", "TAKEOVER"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(cluster_changed);
    assert(slot_map.GetNodeForSlot(0) == replica_node);
    assert(slot_map.GetNodeForSlot(8191) == replica_node);
    assert(slot_map.GetNodeForSlot(16383) == replica_node);
    assert(slot_map.GetNodeState(master_node) == ClusterNodeState::Fail);
    assert(slot_map.FailedNodeCount() == 1);

    std::string after_repl_info = handler.handle(command({"INFO", "replication"}), authenticated);
    assert(after_repl_info.find("role:master") != std::string::npos);
    assert(after_repl_info.find("promoted_from:" + master_node) != std::string::npos);

    std::string nodes = handler.handle(command({"CLUSTER", "NODES"}), authenticated);
    assert(nodes.find(replica_node + " myself,master") != std::string::npos);
    assert(nodes.find(master_node + " master,fail") != std::string::npos);
    assert(nodes.find("0-16383") != std::string::npos);

    assert(handler.handle(command({"SET", "after-failover", "ok"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(cache.get("after-failover").value() == "ok");

    assert(handler.handle(command({"CLUSTER", "FAILOVER", "TAKEOVER"}), authenticated) ==
           RespWriter::simpleString("OK"));

    AppConfig master_config;
    FixedMemoryPool master_pool(4);
    CacheStore master_cache(master_pool);
    ClusterSlotMap master_slot_map;
    master_slot_map.Rebuild({master_node, replica_node});
    std::mutex master_slot_map_mutex;
    CommandHandler master_handler(master_cache, master_pool, master_config, true, master_node,
                                  &master_slot_map, &master_slot_map_mutex);
    bool master_authenticated = true;
    assert(master_handler.handle(command({"CLUSTER", "FAILOVER", "TAKEOVER"}), master_authenticated) ==
           RespWriter::error("CLUSTER FAILOVER is only supported on replicas"));
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

static void testClusterImportingAllowsMigrationWrites() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ClusterSlotMap slot_map;
    const std::string current_node = "127.0.0.1:6366";
    const std::string source_node = "127.0.0.1:6367";
    slot_map.Rebuild({current_node, source_node});
    std::mutex slot_map_mutex;

    const std::string key = "importing-key";
    uint16_t slot = clusterHashSlot(key);
    assert(slot_map.SetSlotOwner(slot, source_node));

    CommandHandler handler(cache, pool, config, true, current_node, &slot_map, &slot_map_mutex);
    CommandSession session;
    session.authenticated = true;
    std::string moved = handler.handle(command({"SET", key, "before"}), session);
    assert(moved.find("-MOVED ") == 0);

    assert(handler.handle(command({"CLUSTER", "SETSLOT", std::to_string(slot), "IMPORTING", source_node}), session) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", key, "blocked"}), session).find("-MOVED ") == 0);
    assert(handler.handle(command({"ASKING"}), session) == RespWriter::simpleString("OK"));
    assert(handler.handle(command({"PING"}), session) == RespWriter::simpleString("PONG"));
    assert(handler.handle(command({"SET", key, "still-blocked"}), session).find("-MOVED ") == 0);
    assert(handler.handle(command({"ASKING"}), session) == RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", key, "after"}), session) ==
           RespWriter::simpleString("OK"));
    assert(cache.get(key).value_or("") == "after");
    assert(handler.handle(command({"SET", key, "again"}), session).find("-MOVED ") == 0);
}

static void testClusterMigratingReturnsAsk() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    ClusterSlotMap slot_map;
    const std::string current_node = "127.0.0.1:6366";
    const std::string target_node = "127.0.0.1:6367";
    slot_map.Rebuild({current_node, target_node});
    std::mutex slot_map_mutex;

    std::string key;
    for (int i = 0; i < 1000; ++i) {
        std::string candidate = "migrating-key-" + std::to_string(i);
        uint16_t slot = clusterHashSlot(candidate);
        if (slot_map.GetNodeForSlot(slot) == current_node) {
            key = candidate;
            assert(slot_map.SetSlotState(slot, ClusterSlotState::Migrating, target_node));
            break;
        }
    }
    assert(!key.empty());

    CommandHandler handler(cache, pool, config, true, current_node, &slot_map, &slot_map_mutex);
    bool authenticated = true;
    uint16_t slot = clusterHashSlot(key);
    std::string response = handler.handle(command({"GET", key}), authenticated);
    std::string expected = "-ASK " + std::to_string(slot) + " " + target_node + "\r\n";
    assert(response == expected);
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

static void testAclRoles() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    AclUser reader;
    reader.username = "reader";
    reader.password = "reader-pass";
    reader.role = AclRole::ReadOnly;
    config.acl_users.push_back(reader);
    AclUser writer;
    writer.username = "writer";
    writer.password = "writer-pass";
    writer.role = AclRole::ReadWrite;
    config.acl_users.push_back(writer);
    AclUser admin;
    admin.username = "admin";
    admin.password = "admin-pass";
    admin.role = AclRole::Admin;
    config.acl_users.push_back(admin);
    AclUser tenant;
    tenant.username = "tenant";
    tenant.password = "tenant-pass";
    tenant.role = AclRole::ReadWrite;
    tenant.command_allowlist_enabled = true;
    tenant.allowed_commands = {"GET", "SET", "ACL"};
    tenant.denied_commands = {"DEL"};
    tenant.all_keys = false;
    tenant.key_prefixes = {"tenant:"};
    config.acl_users.push_back(tenant);
    AclUser disabled;
    disabled.username = "disabled";
    disabled.password = "disabled-pass";
    disabled.role = AclRole::Admin;
    disabled.enabled = false;
    config.acl_users.push_back(disabled);
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);

    CommandSession session;
    assert(handler.handle(command({"PING"}), session) ==
           RespWriter::error("NOAUTH Authentication required"));

    assert(handler.handle(command({"AUTH", "reader", "bad"}), session) ==
           RespWriter::error("invalid password"));
    assert(!session.authenticated);

    assert(handler.handle(command({"AUTH", "reader", "reader-pass"}), session) ==
           RespWriter::simpleString("OK"));
    assert(session.authenticated);
    assert(session.role == AclRole::ReadOnly);
    assert(handler.handle(command({"PING"}), session) == RespWriter::simpleString("PONG"));
    assert(handler.handle(command({"ACL", "WHOAMI"}), session) ==
           RespWriter::bulkString("reader"));
    assert(handler.handle(command({"ACL", "LIST"}), session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));
    assert(handler.handle(command({"SET", "acl:key", "value"}), session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));
    assert(handler.handle(command({"GET", "acl:key"}), session) ==
           RespWriter::nullBulkString());
    assert(handler.handle(command({"STRLEN", "acl:key"}), session) == ":0\r\n");

    assert(handler.handle(command({"AUTH", "writer", "writer-pass"}), session) ==
           RespWriter::simpleString("OK"));
    assert(session.role == AclRole::ReadWrite);
    assert(handler.handle(command({"SET", "acl:key", "value"}), session) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"APPEND", "acl:key", "!"}), session) == ":6\r\n");
    assert(handler.handle(command({"GET", "acl:key"}), session) ==
           RespWriter::bulkString("value!"));
    assert(handler.handle(command({"INCR", "acl:counter"}), session) == ":1\r\n");
    assert(handler.handle(command({"BGREWRITEAOF"}), session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));

    assert(handler.handle(command({"AUTH", "admin", "admin-pass"}), session) ==
           RespWriter::simpleString("OK"));
    assert(session.role == AclRole::Admin);
    std::string acl_list = handler.handle(command({"ACL", "LIST"}), session);
    assert(acl_list.find("reader on role=readonly commands=all keys=* password=*****") != std::string::npos);
    assert(acl_list.find("writer on role=readwrite commands=all keys=* password=*****") != std::string::npos);
    assert(acl_list.find("admin on role=admin commands=all keys=* password=*****") != std::string::npos);
    assert(acl_list.find("tenant on role=readwrite commands=+GET,+SET,+ACL,-DEL keys=tenant:* password=*****") != std::string::npos);
    assert(acl_list.find("disabled off role=admin commands=all keys=* password=*****") != std::string::npos);
    assert(acl_list.find("reader-pass") == std::string::npos);
    std::string tenant_info = handler.handle(command({"ACL", "GETUSER", "tenant"}), session);
    assert(tenant_info.find("tenant") != std::string::npos);
    assert(tenant_info.find("+GET,+SET,+ACL,-DEL") != std::string::npos);
    assert(handler.handle(command({"BGREWRITEAOF"}), session) ==
           RespWriter::error("AOF is not enabled"));

    CommandSession tenant_session;
    assert(handler.handle(command({"AUTH", "tenant", "tenant-pass"}), tenant_session) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "tenant:item", "ok"}), tenant_session) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"GET", "tenant:item"}), tenant_session) ==
           RespWriter::bulkString("ok"));
    assert(handler.handle(command({"SET", "other:item", "bad"}), tenant_session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));
    assert(handler.handle(command({"DEL", "tenant:item"}), tenant_session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));
    assert(handler.handle(command({"INFO"}), tenant_session) ==
           RespWriter::error("NOPERM this user has no permissions to run the command"));

    CommandSession disabled_session;
    assert(handler.handle(command({"AUTH", "disabled", "disabled-pass"}), disabled_session) ==
           RespWriter::error("invalid password"));
    assert(!disabled_session.authenticated);
}

static void testCommandResourceLimits() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    config.max_key_bytes = 3;
    config.max_value_bytes = 4;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "abcd", "v"}), authenticated) ==
           RespWriter::error("key too large"));
    assert(handler.handle(command({"SET", "abc", "12345"}), authenticated) ==
           RespWriter::error("value too large"));
    assert(handler.handle(command({"SET", "abc", "1234"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"GET", "abcd"}), authenticated) ==
           RespWriter::error("key too large"));
    assert(handler.handle(command({"MGET", "abc", "abcd"}), authenticated) ==
           RespWriter::error("key too large"));
    assert(handler.handle(command({"DEL", "abcd"}), authenticated) ==
           RespWriter::error("key too large"));
    assert(handler.handle(command({"CLUSTER", "KEYSLOT", "abcd"}), authenticated) ==
           RespWriter::error("key too large"));

    AppConfig replica_config;
    replica_config.max_key_bytes = 3;
    replica_config.max_value_bytes = 4;
    CommandHandler replica_handler(cache, pool, replica_config, false, "127.0.0.1:6367", nullptr, nullptr);
    assert(replica_handler.handle(command({"REPLSET", "abc", "12345", "0"}), authenticated) ==
           RespWriter::error("value too large"));
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

static void testCommandStringOps() {
    FixedMemoryPool pool(64);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SETNX", "n", "1"}), authenticated) == ":1\r\n");
    assert(handler.handle(command({"SETNX", "n", "2"}), authenticated) == ":0\r\n");
    assert(handler.handle(command({"GET", "n"}), authenticated) ==
           RespWriter::bulkString("1"));

    assert(handler.handle(command({"INCR", "n"}), authenticated) == ":2\r\n");
    assert(handler.handle(command({"INCRBY", "n", "40"}), authenticated) == ":42\r\n");
    assert(handler.handle(command({"DECR", "n"}), authenticated) == ":41\r\n");
    assert(handler.handle(command({"DECRBY", "n", "1"}), authenticated) == ":40\r\n");
    assert(handler.handle(command({"GET", "n"}), authenticated) ==
           RespWriter::bulkString("40"));

    assert(handler.handle(command({"APPEND", "n", "!"}), authenticated) == ":3\r\n");
    assert(handler.handle(command({"STRLEN", "n"}), authenticated) == ":3\r\n");
    assert(handler.handle(command({"GET", "n"}), authenticated) ==
           RespWriter::bulkString("40!"));
    assert(handler.handle(command({"INCR", "n"}), authenticated) ==
           RespWriter::error("value is not an integer or out of range"));

    assert(handler.handle(command({"SET", "max", "9223372036854775807"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"INCR", "max"}), authenticated) ==
           RespWriter::error("increment or decrement would overflow"));

    assert(handler.handle(command({"SET", "ttl-key", "abc", "EX", "10"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"APPEND", "ttl-key", "d"}), authenticated) == ":4\r\n");
    std::string ttl_response = handler.handle(command({"TTL", "ttl-key"}), authenticated);
    assert(ttl_response == ":9\r\n" || ttl_response == ":10\r\n");

    assert(handler.handle(command({"TYPE", "ttl-key"}), authenticated) ==
           RespWriter::simpleString("string"));
    assert(handler.handle(command({"TYPE", "missing"}), authenticated) ==
           RespWriter::simpleString("none"));
    assert(handler.handle(command({"PTTL", "ttl-key"}), authenticated) != ":-1\r\n");
    assert(handler.handle(command({"PERSIST", "ttl-key"}), authenticated) == ":1\r\n");
    assert(handler.handle(command({"TTL", "ttl-key"}), authenticated) == ":-1\r\n");
    assert(handler.handle(command({"PERSIST", "ttl-key"}), authenticated) == ":0\r\n");

    assert(handler.handle(command({"PEXPIRE", "ttl-key", "1500"}), authenticated) == ":1\r\n");
    std::string pttl_response = handler.handle(command({"PTTL", "ttl-key"}), authenticated);
    long long pttl = std::stoll(pttl_response.substr(1, pttl_response.size() - 3));
    assert(pttl > 0 && pttl <= 1500);

    assert(handler.handle(command({"GETEX", "ttl-key", "PERSIST"}), authenticated) ==
           RespWriter::bulkString("abcd"));
    assert(handler.handle(command({"TTL", "ttl-key"}), authenticated) == ":-1\r\n");
    assert(handler.handle(command({"GETEX", "ttl-key", "PX", "1200"}), authenticated) ==
           RespWriter::bulkString("abcd"));
    pttl_response = handler.handle(command({"PTTL", "ttl-key"}), authenticated);
    pttl = std::stoll(pttl_response.substr(1, pttl_response.size() - 3));
    assert(pttl > 0 && pttl <= 1200);

    assert(handler.handle(command({"GETDEL", "ttl-key"}), authenticated) ==
           RespWriter::bulkString("abcd"));
    assert(handler.handle(command({"GET", "ttl-key"}), authenticated) ==
           RespWriter::nullBulkString());
    assert(handler.handle(command({"GETDEL", "ttl-key"}), authenticated) ==
           RespWriter::nullBulkString());
}

static void testCommandSetMaxmemoryOom() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    cache.configure_memory_limit(8, EvictionPolicy::NoEviction);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "a", "123"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SET", "b", "12345"}), authenticated) ==
           RespWriter::error("OOM maxmemory limit reached"));
    assert(handler.handle(command({"EXISTS", "b"}), authenticated) == ":0\r\n");
}

static void testCommandInfo() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    cache.configure_memory_limit(1024, EvictionPolicy::Lru);
    AppConfig config;
    config.maxmemory_bytes = 1024;
    config.eviction_policy = "lru";
    config.snapshot_file = "snapshot_test.dat";
    config.snapshot_interval_sec = 30;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "info-key", "value"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"GET", "info-key"}), authenticated) ==
           RespWriter::bulkString("value"));

    std::string all_info = handler.handle(command({"INFO"}), authenticated);
    assert(all_info.find("# Server") != std::string::npos);
    assert(all_info.find("redis_mode:standalone") != std::string::npos);
    assert(all_info.find("io_threads:4") != std::string::npos);
    assert(all_info.find("max_request_bytes:") != std::string::npos);
    assert(all_info.find("max_pipeline_commands:") != std::string::npos);
    assert(all_info.find("client_output_buffer_limit:") != std::string::npos);
    assert(all_info.find("# Memory") != std::string::npos);
    assert(all_info.find("maxmemory:1024") != std::string::npos);
    assert(all_info.find("maxmemory_policy:lru") != std::string::npos);
    assert(all_info.find("max_key_bytes:") != std::string::npos);
    assert(all_info.find("max_value_bytes:") != std::string::npos);
    assert(all_info.find("cache_shards:1") != std::string::npos);
    assert(all_info.find("# Persistence") != std::string::npos);
    assert(all_info.find("snapshot_format:MINIREDIS_SNAPSHOT_V2") != std::string::npos);
    assert(all_info.find("snapshot_running:") != std::string::npos);
    assert(all_info.find("snapshot_last_duration_ms:") != std::string::npos);
    assert(all_info.find("aof_rewrite_running:") != std::string::npos);
    assert(all_info.find("aof_last_rewrite_records:") != std::string::npos);
    assert(all_info.find("# Replication") != std::string::npos);
    assert(all_info.find("role:master") != std::string::npos);
    assert(all_info.find("# Stats") != std::string::npos);
    assert(all_info.find("keyspace_hits:") != std::string::npos);

    std::string memory_info = handler.handle(command({"INFO", "memory"}), authenticated);
    assert(memory_info.find("# Memory") != std::string::npos);
    assert(memory_info.find("used_memory:") != std::string::npos);
    assert(memory_info.find("cache_shards:1") != std::string::npos);
    assert(memory_info.find("# Server") == std::string::npos);

    assert(handler.handle(command({"INFO", "unknown"}), authenticated) ==
           RespWriter::error("unsupported INFO section 'UNKNOWN'"));
}

static void testCommandReplication() {
    FixedMemoryPool pool(8);
    CacheStore cache(pool);
    AppConfig config;
    config.replicaof = "127.0.0.1:6366";
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6367", nullptr, nullptr);
    bool authenticated = true;

    assert(handler.handle(command({"SET", "client-write", "blocked"}), authenticated) ==
           RespWriter::error("READONLY You can't write against a read only replica"));
    assert(handler.handle(command({"REPLSET", "replicated", "value", "0"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"GET", "replicated"}), authenticated) ==
           RespWriter::bulkString("value"));
    assert(handler.handle(command({"REPLEXPIRE", "replicated", "2"}), authenticated) ==
           RespWriter::simpleString("OK"));
    std::string ttl_response = handler.handle(command({"TTL", "replicated"}), authenticated);
    assert(ttl_response == ":1\r\n" || ttl_response == ":2\r\n");

    std::string snapshot_response = handler.handle(command({"REPLSNAPSHOT"}), authenticated);
    RespDecoder snapshot_decoder;
    snapshot_decoder.feed(snapshot_response);
    auto snapshot_value = snapshot_decoder.parse();
    assert(snapshot_value.has_value());
    assert(snapshot_value->type == RespType::ARRAY);
    std::unordered_map<std::string, SnapshotEntry> snapshot_entries;
    for (const auto& item : snapshot_value->array) {
        assert(item.type == RespType::ARRAY);
        assert(item.array.size() == 3);
        assert(item.array[0].type == RespType::BULK_STRING);
        assert(item.array[1].type == RespType::BULK_STRING);
        assert(item.array[2].type == RespType::INTEGER);
        snapshot_entries[item.array[0].str] = SnapshotEntry{
            item.array[1].str,
            static_cast<uint64_t>(item.array[2].integer),
        };
    }
    assert(snapshot_entries.size() == 1);
    assert(snapshot_entries["replicated"].value == "value");
    assert(snapshot_entries["replicated"].expire_at_ms > unixMillisNow());

    assert(handler.handle(command({"REPLDEL", "replicated"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"GET", "replicated"}), authenticated) ==
           RespWriter::nullBulkString());

    std::string repl_info = handler.handle(command({"INFO", "replication"}), authenticated);
    assert(repl_info.find("# Replication") != std::string::npos);
    assert(repl_info.find("role:slave") != std::string::npos);
    assert(repl_info.find("master_node:127.0.0.1:6366") != std::string::npos);
}

static void testCommandReplicationBacklogPsync() {
    FixedMemoryPool pool(16);
    CacheStore cache(pool);
    AppConfig config;
    ReplicationBacklog backlog(8);
    std::atomic<uint64_t> replica_offset{0};
    uint64_t persisted_offset = 0;
    CommandHandler master(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr,
                          nullptr, {}, &backlog);
    CommandHandler replica(cache, pool, config, false, "127.0.0.1:6367", nullptr, nullptr,
                           nullptr, {}, nullptr, &replica_offset,
                           [&](uint64_t offset) { persisted_offset = offset; });
    bool authenticated = true;

    assert(master.handle(command({"SET", "repl:one", "1"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(master.handle(command({"INCR", "repl:counter"}), authenticated) == ":1\r\n");
    assert(backlog.currentOffset() == 2);
    assert(backlog.size() == 2);

    std::string psync = master.handle(command({"REPLPSYNC", "0"}), authenticated);
    RespDecoder decoder;
    decoder.feed(psync);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 3);
    assert(parsed->array[0].str == "CONTINUE");
    assert(parsed->array[1].integer == 2);
    assert(parsed->array[2].type == RespType::ARRAY);
    assert(parsed->array[2].array.size() == 2);
    assert(parsed->array[2].array[0].array[0].integer == 1);
    assert(parsed->array[2].array[0].array[1].array[0].str == "REPLSET");

    assert(replica.handle(command({"REPLAPPLY", "2", "REPLSET", "applied", "gap", "0"}),
                          authenticated) ==
           RespWriter::error("REPLGAP expected offset 1 but got 2"));
    assert(replica.handle(command({"REPLAPPLY", "1", "REPLSET", "applied", "ok", "0"}),
                          authenticated) == RespWriter::simpleString("OK"));
    assert(replica_offset.load() == 1);
    assert(persisted_offset == 1);
    assert(replica.handle(command({"REPLACK"}), authenticated) == RespWriter::integer(1));
    assert(replica.handle(command({"REPLAPPLY", "1", "REPLSET", "applied", "duplicate", "0"}),
                          authenticated) == RespWriter::simpleString("OK"));
    assert(replica.handle(command({"GET", "applied"}), authenticated) ==
           RespWriter::bulkString("ok"));

    std::string fullsync = master.handle(command({"REPLFULLSYNC"}), authenticated);
    decoder.feed(fullsync);
    parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 2);
    assert(parsed->array[0].integer == 2);
    assert(parsed->array[1].type == RespType::ARRAY);
}

static void testCommandSlowLog() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    Stats::instance().resetSlowLog();
    Stats::instance().configureSlowLog(10, 8);
    Stats::instance().recordCommandLatency(9, std::vector<std::string>{"GET", "fast"});
    Stats::instance().recordCommandLatency(15, std::vector<std::string>{"GET", "slow"});
    Stats::instance().recordCommandLatency(30, std::vector<std::string>{"SET", "slow", "value"});

    assert(handler.handle(command({"SLOWLOG", "LEN"}), authenticated) == ":2\r\n");

    std::string response = handler.handle(command({"SLOWLOG", "GET", "1"}), authenticated);
    RespDecoder decoder;
    decoder.feed(response);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 1);
    const auto& entry = parsed->array[0];
    assert(entry.type == RespType::ARRAY);
    assert(entry.array.size() == 4);
    assert(entry.array[2].integer == 30);
    assert(entry.array[3].type == RespType::ARRAY);
    assert(entry.array[3].array.size() == 3);
    assert(entry.array[3].array[0].str == "SET");
    assert(entry.array[3].array[1].str == "slow");

    assert(handler.handle(command({"SLOWLOG", "RESET"}), authenticated) ==
           RespWriter::simpleString("OK"));
    assert(handler.handle(command({"SLOWLOG", "LEN"}), authenticated) == ":0\r\n");
    assert(handler.handle(command({"SLOWLOG", "BAD"}), authenticated) ==
           RespWriter::error("unsupported SLOWLOG subcommand 'BAD'"));

    Stats::instance().configureSlowLog(10000, 128);
}

static void testCommandMetadata() {
    FixedMemoryPool pool(4);
    CacheStore cache(pool);
    AppConfig config;
    CommandHandler handler(cache, pool, config, false, "127.0.0.1:6366", nullptr, nullptr);
    bool authenticated = true;

    std::string count_response = handler.handle(command({"COMMAND", "COUNT"}), authenticated);
    assert(count_response == ":34\r\n");

    std::string table_response = handler.handle(command({"COMMAND"}), authenticated);
    RespDecoder decoder;
    decoder.feed(table_response);
    auto parsed = decoder.parse();
    assert(parsed.has_value());
    assert(parsed->type == RespType::ARRAY);
    assert(parsed->array.size() == 34);

    bool saw_acl = false;
    bool saw_asking = false;
    bool saw_set = false;
    bool saw_getdel = false;
    bool saw_getex = false;
    bool saw_setnx = false;
    bool saw_strlen = false;
    bool saw_type = false;
    bool saw_append = false;
    bool saw_incr = false;
    bool saw_decr = false;
    bool saw_incrby = false;
    bool saw_decrby = false;
    bool saw_mget = false;
    bool saw_expire = false;
    bool saw_pexpire = false;
    bool saw_persist = false;
    bool saw_ttl = false;
    bool saw_pttl = false;
    bool saw_info = false;
    bool saw_slowlog = false;
    bool saw_cluster = false;
    for (const auto& entry : parsed->array) {
        assert(entry.type == RespType::ARRAY);
        assert(entry.array.size() == 6);
        assert(entry.array[0].type == RespType::BULK_STRING);
        if (entry.array[0].str == "acl") {
            saw_acl = true;
            assert(entry.array[1].integer == -2);
        }
        if (entry.array[0].str == "asking") {
            saw_asking = true;
            assert(entry.array[1].integer == 1);
        }
        if (entry.array[0].str == "set") {
            saw_set = true;
            assert(entry.array[1].integer == -3);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "setnx") {
            saw_setnx = true;
            assert(entry.array[1].integer == 3);
            assert(entry.array[3].integer == 1);
            assert(entry.array[4].integer == 1);
        }
        if (entry.array[0].str == "getdel") {
            saw_getdel = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "getex") {
            saw_getex = true;
            assert(entry.array[1].integer == -2);
        }
        if (entry.array[0].str == "strlen") {
            saw_strlen = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "type") {
            saw_type = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "append") {
            saw_append = true;
            assert(entry.array[1].integer == 3);
        }
        if (entry.array[0].str == "incr") {
            saw_incr = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "decr") {
            saw_decr = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "incrby") {
            saw_incrby = true;
            assert(entry.array[1].integer == 3);
        }
        if (entry.array[0].str == "decrby") {
            saw_decrby = true;
            assert(entry.array[1].integer == 3);
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
        if (entry.array[0].str == "pexpire") {
            saw_pexpire = true;
            assert(entry.array[1].integer == 3);
        }
        if (entry.array[0].str == "persist") {
            saw_persist = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "pttl") {
            saw_pttl = true;
            assert(entry.array[1].integer == 2);
        }
        if (entry.array[0].str == "cluster") {
            saw_cluster = true;
        }
        if (entry.array[0].str == "info") {
            saw_info = true;
            assert(entry.array[1].integer == -1);
        }
        if (entry.array[0].str == "slowlog") {
            saw_slowlog = true;
            assert(entry.array[1].integer == -2);
        }
    }
    assert(saw_acl);
    assert(saw_asking);
    assert(saw_set);
    assert(saw_getdel);
    assert(saw_getex);
    assert(saw_setnx);
    assert(saw_strlen);
    assert(saw_type);
    assert(saw_append);
    assert(saw_incr);
    assert(saw_decr);
    assert(saw_incrby);
    assert(saw_decrby);
    assert(saw_mget);
    assert(saw_expire);
    assert(saw_pexpire);
    assert(saw_persist);
    assert(saw_ttl);
    assert(saw_pttl);
    assert(saw_info);
    assert(saw_slowlog);
    assert(saw_cluster);

    std::string info_response = handler.handle(command({"COMMAND", "INFO", "GET", "SET", "missing"}), authenticated);
    RespDecoder info_decoder;
    info_decoder.feed(info_response);
    auto info = info_decoder.parse();
    assert(info.has_value());
    assert(info->type == RespType::ARRAY);
    assert(info->array.size() == 3);
    assert(info->array[0].type == RespType::ARRAY);
    assert(info->array[0].array[0].str == "get");
    assert(info->array[1].type == RespType::ARRAY);
    assert(info->array[1].array[0].str == "set");
    assert(info->array[2].type == RespType::BULK_STRING);
    assert(info->array[2].str.empty());
}

static void testStatsReadinessExport() {
    Stats::instance().setReady(false);
    Stats::instance().setResourceLimits(1024, 64, 2048, 16, 4096, 128);
    Stats::instance().setSnapshotRunning(true);
    Stats::instance().recordSnapshotResult(true, 3, 7);
    Stats::instance().setSnapshotRunning(false);
    Stats::instance().setAofRewriteRunning(true);
    Stats::instance().setAofRewriteBufferBytes(12);
    Stats::instance().recordAofRewriteResult(true, 2, 9);
    Stats::instance().setAofRewriteRunning(false);
    Stats::instance().setReplicationState(2, 1, 10, 8, 2, 3, 4, 5);
    std::string json = Stats::instance().toJson();
    std::string metrics = Stats::instance().toPrometheus();
    assert(json.find("\"ready\":false") != std::string::npos);
    assert(json.find("\"io_threads\":") != std::string::npos);
    assert(json.find("\"max_request_bytes\":1024") != std::string::npos);
    assert(json.find("\"snapshot_last_key_count\":3") != std::string::npos);
    assert(json.find("\"aof_last_rewrite_records\":2") != std::string::npos);
    assert(json.find("\"aof_rewrite_last_status\":\"ok\"") != std::string::npos);
    assert(json.find("\"replication_connected_replicas\":1") != std::string::npos);
    assert(json.find("\"replication_pending_offsets\":2") != std::string::npos);
    assert(metrics.find("miniredis_ready 0") != std::string::npos);
    assert(metrics.find("miniredis_io_threads") != std::string::npos);
    assert(metrics.find("miniredis_max_request_bytes 1024") != std::string::npos);
    assert(metrics.find("miniredis_snapshot_last_key_count 3") != std::string::npos);
    assert(metrics.find("miniredis_aof_last_rewrite_records 2") != std::string::npos);
    assert(metrics.find("miniredis_aof_rewrite_last_status_info") != std::string::npos);
    assert(metrics.find("miniredis_replication_connected_replicas 1") != std::string::npos);
    assert(metrics.find("miniredis_replication_backlog_misses 5") != std::string::npos);

    Stats::instance().setReady(true);
    json = Stats::instance().toJson();
    metrics = Stats::instance().toPrometheus();
    assert(json.find("\"ready\":true") != std::string::npos);
    assert(metrics.find("miniredis_ready 1") != std::string::npos);
    Stats::instance().setReady(false);
}

int main() {
    testRespParser();
    testRespParserHandlesPartialFrame();
    testRespParserHandlesPipelinedFrames();
    testRespParserRejectsMalformedFrames();
    testMemoryPoolGrow();
    testThreadPoolRejectsSubmitAfterStop();
    testThreadPoolDoesNotShrinkBelowMinimum();
    testLoggerWritesFileAndFiltersLevel();
    testConfigFileParsingAndPrecedence();
    testConfigRejectsRemoteBindWithoutAuth();
    testConfigRejectsMalformedNumbers();
    testCacheStore();
    testCacheStoreShards();
    testCacheStoreMaxmemoryNoEviction();
    testCacheStoreMaxmemoryLru();
    testCacheStoreLazyExpiration();
    testCacheStoreTtlCleanup();
    testCacheStoreTtlQuery();
    testFilePersistence();
    testFilePersistenceLoadsV1Snapshot();
    testFilePersistenceRejectsHugeCount();
    testFilePersistenceBackupRecovery();
    testAppendOnlyFileReplay();
    testAppendOnlyFileIgnoresIncompleteTail();
    testAppendOnlyFileIgnoresInterruptedRewriteTemp();
    testAppendOnlyFileRewriteBufferLimitAllowsRetry();
    testAppendOnlyFileRewrite();
    testCommandHandlerWritesAof();
    testCommandHandlerRewriteAof();
    testCacheStoreSnapshotRestoresTtl();
    testCacheStoreSnapshotSkipsExpiredEntries();
    testClusterHashSlot();
    testClusterSlotMap();
    testClusterConfigStore();
    testClusterCommands();
    testClusterManualFailoverTakeover();
    testMovedCountsAsCommand();
    testClusterRoutesBySlot();
    testClusterImportingAllowsMigrationWrites();
    testClusterMigratingReturnsAsk();
    testAuthFlow();
    testAclRoles();
    testCommandResourceLimits();
    testCommandSetExAndTtl();
    testCommandExpireAndMget();
    testCommandStringOps();
    testCommandSetMaxmemoryOom();
    testCommandInfo();
    testCommandReplication();
    testCommandReplicationBacklogPsync();
    testCommandSlowLog();
    testCommandMetadata();
    testStatsReadinessExport();
    std::cout << "unit tests passed" << std::endl;
    return 0;
}
