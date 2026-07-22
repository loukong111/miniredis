#pragma once

#include <atomic>
#include <string>

namespace miniredis {
void start_stats_http_server(const std::string& bind_addr, int port, std::atomic<bool>& running);
}
