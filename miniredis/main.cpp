#include "miniredis/server/config.hpp"
#include "miniredis/server/server.hpp"
#include <utility>

int main(int argc, char* argv[]) {
    miniredis::AppConfig config;
    miniredis::ConfigParseResult result = miniredis::parseConfig(argc, argv, config);
    if (result == miniredis::ConfigParseResult::Help) {
        return 0;
    }
    if (result == miniredis::ConfigParseResult::Error) {
        return 1;
    }

    miniredis::MiniRedisServer server(std::move(config));
    return server.run();
}
