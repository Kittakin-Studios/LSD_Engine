#include "lsd/foundation/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace lsd::log {

namespace {
std::atomic<Level> g_min_level{Level::Info};
std::atomic<unsigned long long> g_errors{0};
std::mutex g_mutex;
Sink g_sink = nullptr;
void* g_sink_user = nullptr;
FILE* g_file = nullptr;
char g_file_path[260] = "";

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

void set_sink(Sink sink, void* user) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = sink;
    g_sink_user = user;
}

bool open_file(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_file_path[0] = '\0';
    if (!path) return true;
    // Keep the last session's log next to it.
    char previous[300];
    const char* dot = std::strrchr(path, '.');
    const int stem = dot ? static_cast<int>(dot - path) : static_cast<int>(std::strlen(path));
    std::snprintf(previous, sizeof previous, "%.*s-previous%s", stem, path, dot ? dot : "");
    std::remove(previous);
    std::rename(path, previous);
    g_file = std::fopen(path, "wb");
    if (g_file) std::snprintf(g_file_path, sizeof g_file_path, "%s", path);
    return g_file != nullptr;
}

const char* file_path() { return g_file_path; }
unsigned long long error_count() { return g_errors.load(); }

void write(Level level, const char* channel, const char* file, int line, const char* message) {
    if (level >= Level::Error) ++g_errors;
    if (level < g_min_level.load()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* out = level >= Level::Warning ? stderr : stdout;
    if (level >= Level::Error) {
        std::fprintf(out, "[%s][%s] %s (%s:%d)\n", level_name(level), channel, message, file, line);
    } else {
        std::fprintf(out, "[%s][%s] %s\n", level_name(level), channel, message);
    }
    std::fflush(out);
    if (g_file) {
        std::fprintf(g_file, "[%s][%s] %s\n", level_name(level), channel, message);
        std::fflush(g_file);
    }
    if (g_sink) g_sink(g_sink_user, level, channel, message);
}

} // namespace lsd::log
