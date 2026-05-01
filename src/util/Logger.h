#pragma once
// Logger.h — Structured, levelled logging for haiku-act-runner
//
// Features:
//   - Five levels: DEBUG, INFO, WARN, ERROR, FATAL
//   - Timestamps in RFC3339 format
//   - Thread-safe (mutex-protected output)
//   - Configurable minimum level
//   - Optional ANSI colour (auto-detected via isatty())
//
// Usage:
//   LOG_INFO("Poller", "FetchTask returned task " << task_id);
//   LOG_DEBUG("ExprEval", "Evaluating: " << expr);

#include <string>
#include <sstream>
#include <mutex>
#include <cstdio>
#include <atomic>

namespace runner {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
};

class Logger {
public:
    /// Global singleton
    static Logger& instance();

    void setLevel(LogLevel level) { min_level_.store(level); }
    void setLevel(const std::string& level_str);
    void setColour(bool enable) { colour_ = enable; }

    bool isEnabled(LogLevel level) const {
        return level >= min_level_.load();
    }

    void log(LogLevel level,
             const char* component,
             const std::string& message,
             const char* file = nullptr,
             int line = 0);

private:
    Logger();
    std::mutex             mutex_;
    std::atomic<LogLevel>  min_level_{LogLevel::Info};
    bool                   colour_ = false;

    static const char* levelStr(LogLevel level);
    static const char* levelColour(LogLevel level);
    static std::string  timestamp();
};

} // namespace runner

// ─── Logging macros ───────────────────────────────────────────────────────
// Use like: LOG_INFO("Component", "message " << variable);

#define LOG_AT(level, component, msg) \
    do { \
        if (::runner::Logger::instance().isEnabled(level)) { \
            std::ostringstream _oss; \
            _oss << msg; \
            ::runner::Logger::instance().log(level, component, _oss.str(), \
                                              __FILE__, __LINE__); \
        } \
    } while(0)

#define LOG_DEBUG(comp, msg) LOG_AT(::runner::LogLevel::Debug, comp, msg)
#define LOG_INFO(comp, msg)  LOG_AT(::runner::LogLevel::Info,  comp, msg)
#define LOG_WARN(comp, msg)  LOG_AT(::runner::LogLevel::Warn,  comp, msg)
#define LOG_ERROR(comp, msg) LOG_AT(::runner::LogLevel::Error, comp, msg)
#define LOG_FATAL(comp, msg) LOG_AT(::runner::LogLevel::Fatal, comp, msg)
