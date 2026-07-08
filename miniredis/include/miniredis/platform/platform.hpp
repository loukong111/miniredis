#pragma once

namespace miniredis::platform {

#if defined(__linux__)
inline constexpr bool kLinux = true;
inline constexpr bool kServerSupported = true;
inline constexpr const char* kServerPlatform = "Linux";
#else
inline constexpr bool kLinux = false;
inline constexpr bool kServerSupported = false;
inline constexpr const char* kServerPlatform = "unsupported";
#endif

inline constexpr const char* kServerPortabilityNote =
    "MiniRedis server currently uses epoll/eventfd/POSIX sockets and is Linux-first. "
    "The Qt console is designed to be built as a cross-platform management client.";

}  // namespace miniredis::platform
