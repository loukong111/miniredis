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
    "MiniRedis server targets Linux because its reactor backend uses epoll/eventfd/POSIX sockets. "
    "Use Docker or a remote Linux server with the cross-platform Qt console on Windows/macOS.";

}  // namespace miniredis::platform
