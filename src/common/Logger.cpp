#include "common/Logger.h"
#include <iostream>
#include <iomanip>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ndi_bridge {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger()
    : startTime_(std::chrono::steady_clock::now())
{
    // Auto-detect if we're in a terminal
#ifdef _WIN32
    colored_ = _isatty(_fileno(stdout));
#else
    colored_ = isatty(STDOUT_FILENO);
#endif
}

void Logger::setVerbose(bool verbose) {
    verbose_ = verbose;
}

void Logger::setColored(bool colored) {
    colored_ = colored;
}

void Logger::setTimestamps(bool timestamps) {
    timestamps_ = timestamps;
}

void Logger::debug(std::string_view message) {
    if (verbose_) {
        logImpl(LogLevel::Debug, message);
    }
}

void Logger::info(std::string_view message) {
    logImpl(LogLevel::Info, message);
}

void Logger::success(std::string_view message) {
    logImpl(LogLevel::Success, message);
}

void Logger::error(std::string_view message) {
    logImpl(LogLevel::Error, message);
}

void Logger::log(LogLevel level, std::string_view message) {
    if (level == LogLevel::Debug && !verbose_) {
        return;
    }
    logImpl(level, message);
}

void Logger::logImpl(LogLevel level, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;

    // Timestamp
    if (timestamps_) {
        out << formatTimestamp() << " ";
    }

    // Color and prefix
    if (colored_) {
        out << levelColor(level);
    }
    out << levelPrefix(level);
    if (colored_) {
        out << color::Reset;
    }

    // Message
    out << " " << message << "\n";
    out.flush();
}

std::string Logger::formatTimestamp() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_);

    auto hours = std::chrono::duration_cast<std::chrono::hours>(elapsed);
    elapsed -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed);
    elapsed -= minutes;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    elapsed -= seconds;
    auto millis = elapsed.count();

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "[%02lld:%02lld:%02lld.%03lld]",
                  static_cast<long long>(hours.count() % 24),
                  static_cast<long long>(minutes.count()),
                  static_cast<long long>(seconds.count()),
                  static_cast<long long>(millis));
    return buffer;
}

const char* Logger::levelColor(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug:   return color::Cyan;
        case LogLevel::Info:    return color::White;
        case LogLevel::Success: return color::Green;
        case LogLevel::Error:   return color::Red;
    }
    return color::Reset;
}

const char* Logger::levelPrefix(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug:   return "[DEBUG]";
        case LogLevel::Info:    return "[INFO]";
        case LogLevel::Success: return "[OK]";
        case LogLevel::Error:   return "[ERROR]";
    }
    return "[???]";
}

} // namespace ndi_bridge
