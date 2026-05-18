#include "stats.hpp"
#include "http_stats.hpp"
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

void start_stats_http_server(int port, std::atomic<bool>& running) {
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
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("stats server bind");
        close(server_fd);
        return;
    }
    if (listen(server_fd, 5) < 0) {
        perror("stats server listen");
        close(server_fd);
        return;
    }
    std::cout << "Stats HTTP server listening on port " << port << std::endl;

    while (running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("stats server accept");
            break;
        }
        handle_client(client_fd);
    }
    close(server_fd);
}

}