#include "lsd/core/crash.h"

#include "lsd/foundation/log.h"

#include <cstdio>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: dbghelp.h needs its types.
#include <dbghelp.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace lsd {

namespace {

char g_folder[260] = "";
char g_marker[300] = "";

// Saved/Crashes/2026-10-30_15-04-05: made at crash time (the stack is still usable for that much).
void crash_dir(char* out, usize capacity) {
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    std::snprintf(out, capacity, "%s/%04d-%02d-%02d_%02d-%02d-%02d", g_folder, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

void save_log_and_marker(const char* dir) {
    std::error_code error;
    if (const char* log = log::file_path(); log[0]) std::filesystem::copy_file(log, std::filesystem::path(dir) / "log.txt", std::filesystem::copy_options::overwrite_existing, error);
    if (FILE* m = std::fopen(g_marker, "wb")) {
        std::fputs(dir, m);
        std::fclose(m);
    }
}

#ifdef _WIN32
LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    char dir[300];
    crash_dir(dir, sizeof dir);
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    char dump_path[340];
    std::snprintf(dump_path, sizeof dump_path, "%s/crash.dmp", dir);
    HANDLE file = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
                          info ? &exception : nullptr, nullptr, nullptr);
        CloseHandle(file);
    }
    LSD_LOG_ERROR("crash", "Crashed (exception 0x%08lX); report in %s", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul, dir);
    save_log_and_marker(dir);
    return EXCEPTION_EXECUTE_HANDLER;  // end the process (no Windows error dialog)
}
#else
void on_signal(int signal) {
    char dir[300];
    crash_dir(dir, sizeof dir);
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    LSD_LOG_ERROR("crash", "Crashed (signal %d); report in %s", signal, dir);
    save_log_and_marker(dir);
    _exit(1);
}
#endif

} // namespace

void install_crash_reporting(const char* crash_folder, char* previous, usize capacity) {
    std::snprintf(g_folder, sizeof g_folder, "%s", crash_folder);
    std::snprintf(g_marker, sizeof g_marker, "%s/last_crash.txt", crash_folder);
    std::error_code error;
    std::filesystem::create_directories(crash_folder, error);
    previous[0] = '\0';
    if (FILE* m = std::fopen(g_marker, "rb")) {
        const usize n = std::fread(previous, 1, capacity - 1, m);
        previous[n] = '\0';
        std::fclose(m);
        std::remove(g_marker);
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
#else
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);
    std::signal(SIGFPE, on_signal);
    std::signal(SIGILL, on_signal);
#endif
}

} // namespace lsd
