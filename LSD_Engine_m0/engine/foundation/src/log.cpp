#include "lsd/foundation/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace lsd::log {

namespace {
std::atomic<Level> g_min_level{Level::Info};
std::mutex g_mutex;

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace:   return "TRACE";
        case Level::Info:    return "INFO ";
        case Level::Warning: return "WARN ";
        case Level::Error:   return "ERROR";
        case Level::Fatal:   return "FATAL";
    }
    return "?";
}
} // namespace

void set_min_level(Level level) { g_min_level.store(level); }

void write(Level level, const char* channel, const char* file, int line, const char* message) {
    if (level < g_min_level.load()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* out = level >= Level::Warning ? stderr : stdout;
    if (level >= Level::Error) {
        std::fprintf(out, "[%s][%s] %s (%s:%d)\n", level_name(level), channel, message, file, line);
    } else {
        std::fprintf(out, "[%s][%s] %s\n", level_name(level), channel, message);
    }
    std::fflush(out);
}

} // namespace lsd::log
