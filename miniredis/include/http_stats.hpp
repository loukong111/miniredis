#pragma once

#include <atomic>

namespace miniredis {
void start_stats_http_server(int port, std::atomic<bool>& running);
}