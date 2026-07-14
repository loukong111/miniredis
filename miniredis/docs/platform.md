# 平台支持

MiniRedis 的服务端定位是 Linux 内网服务端，管理端定位是跨平台 Qt Console。

## 支持矩阵

| 能力 | Linux | Windows | macOS |
|---|---:|---:|---:|
| 原生 MiniRedis 服务端 | 支持 | 不支持 | 不支持 |
| Qt Console 管理端 | 支持 | 支持 | 支持 |
| Docker 运行服务端 | 支持 | 支持 Docker Desktop | 支持 Docker Desktop |
| systemd 部署 | 支持 | 不适用 | 不适用 |

## 为什么服务端选择 Linux-first

服务端网络层使用 `epoll`、`eventfd` 和 POSIX socket，实现多 Reactor、协程 IO 等能力。这个选择更贴近 Linux 后端服务的真实部署环境，也能让网络模型保持简单、可解释和高性能。

Windows/macOS 原生服务端需要重新抽象 IO backend，例如 Windows IOCP、macOS kqueue，改动会影响调度器、连接生命周期、事件唤醒和集成测试。当前项目优先保证 Linux 服务端质量，把跨平台能力放在 Qt Console 管理端和 Docker 运行路径上。

## 推荐部署方式

- Linux 服务器：原生二进制、systemd 或 Docker。
- Windows/macOS 开发机：运行 Qt Console，通过 RESP/HTTP 连接 Linux 或 Docker 中的 MiniRedis 服务端。
- 跨平台演示：本地 Docker 启动服务端，本机 Qt Console 连接服务端。

## 构建边界

Linux 服务端构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Qt Console-only 构建：

```bash
cmake -S . -B build-qt \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_SERVER=OFF \
  -DMINIREDIS_BUILD_TESTS=OFF \
  -DMINIREDIS_BUILD_QT_CONSOLE=ON

cmake --build build-qt -j
```

平台边界检查：

```bash
scripts/check_platform_build.sh
```

如果在 Windows/macOS 强制开启 `MINIREDIS_BUILD_SERVER=ON`，CMake 会直接报错，避免生成半可用的服务端构建。
