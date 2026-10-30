#pragma once
#include <cstdio>
#include <utility>

namespace lsd::log {

enum class Level { Trace, Info, Warning, Error, Fatal };

// Minimum level that is printed. Defaults to Info.
void set_min_level(Level level);

// Extra receiver for every printed message (the editor's Output Log). Called under the log lock,
// from any thread; keep it short. One sink; pass nullptr to remove it.
using Sink = void (*)(void* user, Level level, const char* channel, const char* message);
void set_sink(Sink sink, void* user);

// Also writes every line to this file (flushed per line, so a crash keeps everything up to it);
// the previous file is kept as <name>-previous.<ext>. nullptr closes it. False if it cannot be opened.
bool open_file(const char* path);
const char* file_path();  // "" when there is none
// How many errors (and fatals) have been written so far (smoke tests compare before and after).
unsigned long long error_count();

// Low-level entry point; prefer the LSD_LOG_* macros.
void write(Level level, const char* channel, const char* file, int line, const char* message);

template <typename... Args>
void writef(Level level, const char* channel, const char* file, int line, const char* fmt, Args&&... args) {
    char buffer[1024];
    if constexpr (sizeof...(Args) == 0) {
        std::snprintf(buffer, sizeof buffer, "%s", fmt);
    } else {
        std::snprintf(buffer, sizeof buffer, fmt, std::forward<Args>(args)...);
    }
    write(level, channel, file, line, buffer);
}

} // namespace lsd::log

#define LSD_LOG_INFO(channel, ...)  ::lsd::log::writef(::lsd::log::Level::Info,    channel, __FILE__, __LINE__, __VA_ARGS__)
#define LSD_LOG_WARN(channel, ...)  ::lsd::log::writef(::lsd::log::Level::Warning, channel, __FILE__, __LINE__, __VA_ARGS__)
#define LSD_LOG_ERROR(channel, ...) ::lsd::log::writef(::lsd::log::Level::Error,   channel, __FILE__, __LINE__, __VA_ARGS__)
