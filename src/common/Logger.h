#pragma once

/**
 * Logger.h - Simple logging system for NDI Bridge
 *
 * Provides colored console output with log levels:
 *   - error:   Red, always shown
 *   - success: Green, always shown
 *   - info:    Default, always shown
 *   - debug:   Cyan, only when verbose mode enabled
 */

#include <string>
#include <string_view>
#include <mutex>
#include <chrono>

namespace ndi_bridge {

enum class LogLevel {
    Debug,
    Info,
    Success,
    Error
};

class Logger {
public:
    /**
     * Get the global logger instance
     */
    static Logger& instance();

    /**
     * Enable/disable verbose (debug) output
     */
    void setVerbose(bool verbose);
    bool isVerbose() const { return verbose_; }

    /**
     * Enable/disable colored output
     */
    void setColored(bool colored);
    bool isColored() const { return colored_; }

    /**
     * Enable/disable timestamps
     */
    void setTimestamps(bool timestamps);
    bool hasTimestamps() const { return timestamps_; }

    /**
     * Log methods
     */
    void debug(std::string_view message);
    void info(std::string_view message);
    void success(std::string_view message);
    void error(std::string_view message);

    /**
     * Formatted log methods (printf-style)
     */
    template<typename... Args>
    void debugf(const char* format, Args... args);

    template<typename... Args>
    void infof(const char* format, Args... args);

    template<typename... Args>
    void successf(const char* format, Args... args);

    template<typename... Args>
    void errorf(const char* format, Args... args);

    /**
     * Log with explicit level
     */
    void log(LogLevel level, std::string_view message);

private:
    Logger();
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void logImpl(LogLevel level, std::string_view message);
    std::string formatTimestamp() const;
    const char* levelColor(LogLevel level) const;
    const char* levelPrefix(LogLevel level) const;

    std::mutex mutex_;
    bool verbose_ = false;
    bool colored_ = true;
    bool timestamps_ = false;
    std::chrono::steady_clock::time_point startTime_;
};

// ANSI color codes
namespace color {
    constexpr const char* Reset   = "\033[0m";
    constexpr const char* Red     = "\033[31m";
    constexpr const char* Green   = "\033[32m";
    constexpr const char* Yellow  = "\033[33m";
    constexpr const char* Blue    = "\033[34m";
    constexpr const char* Magenta = "\033[35m";
    constexpr const char* Cyan    = "\033[36m";
    constexpr const char* White   = "\033[37m";
    constexpr const char* Bold    = "\033[1m";
    constexpr const char* Dim     = "\033[2m";
}

// Convenience macros
#define LOG_DEBUG(msg)   ndi_bridge::Logger::instance().debug(msg)
#define LOG_INFO(msg)    ndi_bridge::Logger::instance().info(msg)
#define LOG_SUCCESS(msg) ndi_bridge::Logger::instance().success(msg)
#define LOG_ERROR(msg)   ndi_bridge::Logger::instance().error(msg)

// Template implementations
template<typename... Args>
void Logger::debugf(const char* format, Args... args) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    debug(buffer);
}

template<typename... Args>
void Logger::infof(const char* format, Args... args) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    info(buffer);
}

template<typename... Args>
void Logger::successf(const char* format, Args... args) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    success(buffer);
}

template<typename... Args>
void Logger::errorf(const char* format, Args... args) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    error(buffer);
}

} // namespace ndi_bridge
