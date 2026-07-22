#include "miniredis/core/logger.hpp"
#include "miniredis/platform/platform.hpp"
#include "miniredis/server/config.hpp"
#include "miniredis/server/server.hpp"
#include <iostream>
#include <utility>

static_assert(miniredis::platform::kServerSupported,
              "MiniRedis server currently requires Linux. Build the Qt console on non-Linux platforms.");

int main(int argc, char* argv[]) {
    miniredis::AppConfig config;
    miniredis::ConfigParseResult result = miniredis::parseConfig(argc, argv, config);
    if (result == miniredis::ConfigParseResult::Help) {
        return 0;
    }
    if (result == miniredis::ConfigParseResult::Error) {
        return 1;
    }

    miniredis::LogLevel log_level = miniredis::LogLevel::Info;
    if (!miniredis::parseLogLevel(config.log_level, log_level)) {
        std::cerr << "Invalid log level: " << config.log_level << std::endl;
        return 1;
    }
    if (!miniredis::Logger::instance().configure(log_level, config.log_file)) {
        return 1;
    }

    miniredis::MiniRedisServer server(std::move(config));
    return server.run();
}
