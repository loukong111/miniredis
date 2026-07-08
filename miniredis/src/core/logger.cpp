#include "miniredis/core/logger.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace miniredis {
namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

bool Logger::configure(LogLevel level, const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
    if (file_.is_open()) {
        file_.close();
    }
    if (!file_path.empty()) {
        file_.open(file_path, std::ios::app);
        if (!file_) {
            std::cerr << "Failed to open log file: " << file_path << std::endl;
            return false;
        }
    }
    return true;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

bool Logger::shouldLog(LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(level) >= static_cast<int>(level_);
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }

    std::ostringstream line;
    line << currentTimestamp()
         << " [" << logLevelName(level) << "]"
         << " [" << module << "] "
         << message << '\n';

    if (file_.is_open()) {
        file_ << line.str();
        file_.flush();
    } else {
        std::cerr << line.str();
    }
}

bool parseLogLevel(const std::string& text, LogLevel& out) {
    std::string value = lowerCopy(text);
    if (value == "debug") {
        out = LogLevel::Debug;
    } else if (value == "info") {
        out = LogLevel::Info;
    } else if (value == "warn" || value == "warning") {
        out = LogLevel::Warn;
    } else if (value == "error") {
        out = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

LogLine::LogLine(LogLevel level, const char* module)
    : level_(level),
      module_(module),
      enabled_(Logger::instance().shouldLog(level)) {}

LogLine::~LogLine() {
    if (enabled_) {
        Logger::instance().log(level_, module_, stream_.str());
    }
}

} // namespace miniredis
