#pragma once

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace miniredis {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    bool configure(LogLevel level, const std::string& file_path);
    void setLevel(LogLevel level);
    LogLevel level() const;
    bool shouldLog(LogLevel level) const;
    void log(LogLevel level, const std::string& module, const std::string& message);

private:
    Logger() = default;

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::ofstream file_;
};

bool parseLogLevel(const std::string& text, LogLevel& out);
const char* logLevelName(LogLevel level);

class LogLine {
public:
    LogLine(LogLevel level, const char* module);
    ~LogLine();

    template <typename T>
    LogLine& operator<<(const T& value) {
        if (enabled_) {
            stream_ << value;
        }
        return *this;
    }

private:
    LogLevel level_;
    const char* module_;
    bool enabled_;
    std::ostringstream stream_;
};

} // namespace miniredis

#define MINIREDIS_LOG(level, module) miniredis::LogLine((level), (module))
#define MINIREDIS_LOG_DEBUG(module) MINIREDIS_LOG(miniredis::LogLevel::Debug, (module))
#define MINIREDIS_LOG_INFO(module) MINIREDIS_LOG(miniredis::LogLevel::Info, (module))
#define MINIREDIS_LOG_WARN(module) MINIREDIS_LOG(miniredis::LogLevel::Warn, (module))
#define MINIREDIS_LOG_ERROR(module) MINIREDIS_LOG(miniredis::LogLevel::Error, (module))
