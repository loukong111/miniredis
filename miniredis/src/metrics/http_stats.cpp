#include "miniredis/metrics/stats.hpp"
#include "miniredis/metrics/http_stats.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>
#include <string>
#include <string_view>

namespace miniredis {

namespace {

constexpr size_t kMaxHttpRequestBytes = 8192;
constexpr int kHttpIoTimeoutSec = 2;

void set_client_timeouts(int client_fd) {
    timeval timeout{};
    timeout.tv_sec = kHttpIoTimeoutSec;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool read_http_request(int client_fd, std::string& request) {
    char buffer[1024];

    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < kMaxHttpRequestBytes) {
        size_t remaining = kMaxHttpRequestBytes - request.size();
        ssize_t n = read(client_fd, buffer, std::min(sizeof(buffer), remaining));
        if (n > 0) {
            request.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return !request.empty();
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return !request.empty();
        return false;
    }

    return !request.empty();
}

bool write_all(int client_fd, const std::string& response) {
    size_t written = 0;
    while (written < response.size()) {
        ssize_t n = write(client_fd, response.data() + written, response.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::string make_response(const std::string& status, const std::string& content_type,
                          const std::string& body) {
    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + content_type + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

bool request_line_matches(const std::string& request, const std::string& method,
                          const std::string& path) {
    size_t line_end = request.find("\r\n");
    std::string_view line(request.data(), line_end == std::string::npos ? request.size() : line_end);
    std::string expected = method + " " + path + " ";
    return line.rfind(expected, 0) == 0;
}

} // namespace

static void handle_client(int client_fd) {
    set_client_timeouts(client_fd);

    std::string request;
    if (!read_http_request(client_fd, request)) {
        close(client_fd);
        return;
    }

    if (request.find("\r\n\r\n") == std::string::npos &&
        request.size() >= kMaxHttpRequestBytes) {
        const std::string response = make_response("431 Request Header Fields Too Large",
                                                   "text/plain", "request header too large\n");
        write_all(client_fd, response);
    } else if (request_line_matches(request, "GET", "/stats")) {
        std::string body = Stats::instance().toJson();
        std::string response = make_response("200 OK", "application/json", body);
        write_all(client_fd, response);
    } else {
        const std::string response = make_response("404 Not Found", "text/plain", "not found\n");
        write_all(client_fd, response);
    }
    close(client_fd);
}

void start_stats_http_server(const std::string& bind_addr, int port, std::atomic<bool>& running) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("stats server socket");
        return;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid stats bind address: " << bind_addr << std::endl;
        close(server_fd);
        return;
    }
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("stats server bind");
        close(server_fd);
        return;
    }
    if (listen(server_fd, 128) < 0) {
        perror("stats server listen");
        close(server_fd);
        return;
    }
    fcntl(server_fd, F_SETFL, fcntl(server_fd, F_GETFL) | O_NONBLOCK);
    std::cout << "Stats HTTP server listening on " << bind_addr << ":" << port << std::endl;

    while (running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            perror("stats server accept");
            break;
        }
        // 同步写法，stats HTTP server 一次只处理一个客户端。
        // 因为 stats HTTP 服务非常轻量，不是主业务服务，所以不考虑 epoll。
        handle_client(client_fd);
    }
    close(server_fd);
}

}
