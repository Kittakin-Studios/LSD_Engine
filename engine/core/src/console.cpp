#include "lsd/core/console.h"
#include "lsd/foundation/log.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsd::console {

namespace {

enum class Kind : u8 { Variable, Command };

struct Entry {
    const char* name;
    const char* help;
    Kind kind;
    VarType type;
    void* target;  // variable value, or command user data
    Command command;
};

// shortcut: fixed table, linear search; a hash map when there are thousands of variables.
constexpr u32 kMaxEntries = 512;
Entry g_entries[kMaxEntries];
u32 g_count = 0;

bool same_ignore_case(const char* a, const char* b, usize n) {
    for (usize i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        if (!a[i]) return true;
    }
    return true;
}

Entry* find(const char* name, usize length) {
    for (u32 i = 0; i < g_count; ++i) {
        if (std::strlen(g_entries[i].name) == length && same_ignore_case(g_entries[i].name, name, length)) return &g_entries[i];
    }
    return nullptr;
}

void add(const Entry& entry) {
    if (Entry* existing = find(entry.name, std::strlen(entry.name))) {
        *existing = entry;  // re-registering replaces (hot reload, tests)
        return;
    }
    if (g_count == kMaxEntries) {
        LSD_LOG_ERROR("console", "Too many console entries; '%s' not added", entry.name);
        return;
    }
    g_entries[g_count++] = entry;
}

void print_value(const Entry& e) {
    switch (e.type) {
        case VarType::Bool: LSD_LOG_INFO("console", "%s = %d   (%s)", e.name, *static_cast<bool*>(e.target) ? 1 : 0, e.help); break;
        case VarType::I32: LSD_LOG_INFO("console", "%s = %d   (%s)", e.name, *static_cast<i32*>(e.target), e.help); break;
        case VarType::F32: LSD_LOG_INFO("console", "%s = %g   (%s)", e.name, static_cast<f64>(*static_cast<f32*>(e.target)), e.help); break;
    }
}

bool set_value(const Entry& e, const char* text) {
    char* end = nullptr;
    errno = 0;
    switch (e.type) {
        case VarType::Bool: {
            const bool on = std::strcmp(text, "1") == 0 || same_ignore_case(text, "true", 5) || same_ignore_case(text, "on", 3);
            const bool off = std::strcmp(text, "0") == 0 || same_ignore_case(text, "false", 6) || same_ignore_case(text, "off", 4);
            if (!on && !off) return false;
            *static_cast<bool*>(e.target) = on;
            return true;
        }
        case VarType::I32: {
            const long v = std::strtol(text, &end, 10);
            if (*end || errno || v < INT32_MIN || v > INT32_MAX) return false;
            *static_cast<i32*>(e.target) = static_cast<i32>(v);
            return true;
        }
        case VarType::F32: {
            const f32 v = std::strtof(text, &end);
            if (*end || errno) return false;
            *static_cast<f32*>(e.target) = v;
            return true;
        }
    }
    return false;
}

} // namespace

void add_variable(const char* name, bool* value, const char* help) { add({name, help, Kind::Variable, VarType::Bool, value, nullptr}); }
void add_variable(const char* name, i32* value, const char* help) { add({name, help, Kind::Variable, VarType::I32, value, nullptr}); }
void add_variable(const char* name, f32* value, const char* help) { add({name, help, Kind::Variable, VarType::F32, value, nullptr}); }
void add_command(const char* name, Command command, void* user, const char* help) { add({name, help, Kind::Command, VarType::Bool, user, command}); }
void remove_all() { g_count = 0; }

bool execute(const char* line) {
    while (*line == ' ' || *line == '\t') ++line;
    usize n = 0;
    while (line[n] && line[n] != ' ' && line[n] != '\t') ++n;
    if (n == 0) return true;
    // Arguments: the rest of the line, trimmed.
    char args[16384];  // long enough for a Lua chunk sent by a tool
    const char* a = line + n;
    while (*a == ' ' || *a == '\t') ++a;
    std::snprintf(args, sizeof args, "%s", a);
    for (usize len = std::strlen(args); len > 0 && (args[len - 1] == ' ' || args[len - 1] == '\t' || args[len - 1] == '\n' || args[len - 1] == '\r');) args[--len] = '\0';

    const Entry* e = find(line, n);
    if (!e) {
        LSD_LOG_WARN("console", "Unknown command '%.*s' (type help)", static_cast<int>(n), line);
        return false;
    }
    if (e->kind == Kind::Command) {
        e->command(e->target, args);
        return true;
    }
    if (!args[0]) {
        print_value(*e);
        return true;
    }
    if (!set_value(*e, args)) {
        LSD_LOG_WARN("console", "Bad value '%s' for %s", args, e->name);
        return false;
    }
    print_value(*e);
    return true;
}

u32 complete(const char* prefix, const char** out, u32 max) {
    const usize n = std::strlen(prefix);
    u32 found = 0;
    for (u32 i = 0; i < g_count && found < max; ++i) {
        if (same_ignore_case(g_entries[i].name, prefix, n)) out[found++] = g_entries[i].name;
    }
    return found;
}

const char* help(const char* name) {
    const Entry* e = find(name, std::strlen(name));
    return e ? e->help : nullptr;
}

} // namespace lsd::console
