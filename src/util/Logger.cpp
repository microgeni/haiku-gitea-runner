// Logger.cpp — thread-safe levelled logging
#include "Logger.h"

#include <ctime>
#include <cstring>
#include <iostream>
#include <unistd.h>   // isatty()
#include <algorithm>
#include <cctype>

namespace runner {

// ─── Logger singleton ─────────────────────────────────────────────────────

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() {
    // Auto-detect ANSI colour support
    colour_ = isatty(STDERR_FILENO);
}

// ─── setLevel (string) ────────────────────────────────────────────────────

void Logger::setLevel(const std::string& level_str) {
    std::string l = level_str;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if      (l == "debug") min_level_.store(LogLevel::Debug);
    else if (l == "info")  min_level_.store(LogLevel::Info);
    else if (l == "warn" || l == "warning") min_level_.store(LogLevel::Warn);
    else if (l == "error") min_level_.store(LogLevel::Error);
    else if (l == "fatal") min_level_.store(LogLevel::Fatal);
    // Unknown level: keep current
}

// ─── Formatting helpers ───────────────────────────────────────────────────

const char* Logger::levelStr(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
        case LogLevel::Fatal: return "FTL";
    }
    return "???";
}

const char* Logger::levelColour(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[36m";   // cyan
        case LogLevel::Info:  return "\033[32m";   // green
        case LogLevel::Warn:  return "\033[33m";   // yellow
        case LogLevel::Error: return "\033[31m";   // red
        case LogLevel::Fatal: return "\033[35m";   // magenta
    }
    return "";
}

std::string Logger::timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    gmtime_r(&ts.tv_sec, &tm_info);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);

    // Append milliseconds.  tv_nsec is in [0, 999999999], so ms ∈ [0, 999];
    // clamp explicitly so the compiler can prove the output fits in 7 bytes.
    unsigned ms = static_cast<unsigned>(ts.tv_nsec / 1'000'000) % 1000u;
    char ms_buf[8];
    snprintf(ms_buf, sizeof(ms_buf), ".%03uZ", ms);
    return std::string(buf) + ms_buf;
}

// ─── log() ────────────────────────────────────────────────────────────────

void Logger::log(LogLevel level,
                  const char* component,
                  const std::string& message,
                  const char* /*file*/,
                  int /*line*/)
{
    std::string ts  = timestamp();
    const char* lvl = levelStr(level);

    std::lock_guard<std::mutex> lock(mutex_);

    if (colour_) {
        const char* col   = levelColour(level);
        const char* reset = "\033[0m";
        const char* dim   = "\033[2m";
        fprintf(stderr, "%s%s%s %s%-3s%s %s%-14s%s %s\n",
                dim, ts.c_str(), reset,
                col, lvl, reset,
                dim, component, reset,
                message.c_str());
    } else {
        fprintf(stderr, "%s %-3s %-14s %s\n",
                ts.c_str(), lvl, component, message.c_str());
    }

    if (level == LogLevel::Fatal) {
        fflush(stderr);
        std::abort();
    }
}

} // namespace runner
