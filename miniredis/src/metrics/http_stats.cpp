#include "miniredis/metrics/stats.hpp"
#include "miniredis/metrics/http_stats.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>

namespace miniredis {

static void handle_client(int client_fd) {
    char buffer[1024];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buffer[n] = '\0';
    // 简单解析请求行，只处理 GET /stats
    if (strncmp(buffer, "GET /stats", 10) == 0) {
        std::string body = Stats::instance().toJson();
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n"
                               "\r\n" + body;
        write(client_fd, response.data(), response.size());
    } else {
        const char* not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
        write(client_fd, not_found, strlen(not_found));
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
        handle_client(client_fd);
    }
    close(server_fd);
}

}
