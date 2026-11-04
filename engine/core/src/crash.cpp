#include "lsd/core/crash.h"

#include "lsd/foundation/log.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
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
// The calling thread's stack, function / file / line per frame, into the log (dbghelp reads the PDB).
void log_stack() {
    void* frames[48];
    const USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    for (USHORT i = 0; i < count; ++i) {
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD displacement = 0;
        const bool named = SymFromAddr(process, address, nullptr, symbol);
        const bool lined = SymGetLineFromAddr64(process, address, &displacement, &line);
        LSD_LOG_ERROR("crash", "  at %s  %s:%lu", named ? symbol->Name : "?", lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
    }
}

void write_report(EXCEPTION_POINTERS* info, const char* why) {
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
    LSD_LOG_ERROR("crash", "%s; report in %s", why, dir);
    log_stack();
    save_log_and_marker(dir);
}

// abort() and std::terminate (a thread left running, an exception out of a destructor): a report
// with the stack instead of the C runtime's "abort() has been called" box.
[[noreturn]] void on_terminate() {
    char why[300] = "Terminated (std::terminate)";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            std::snprintf(why, sizeof why, "Terminated by an uncaught exception: %s", x.what());
        } catch (...) {
            std::snprintf(why, sizeof why, "Terminated by an uncaught exception of an unknown type");
        }
    }
    write_report(nullptr, why);
    std::_Exit(3);
}

void on_abort(int) {
    write_report(nullptr, "Aborted (abort() was called)");
    std::_Exit(3);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    char why[64];
    std::snprintf(why, sizeof why, "Crashed (exception 0x%08lX)", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul);
    write_report(info, why);
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
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // our report instead of the runtime's dialog
#else
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);
    std::signal(SIGFPE, on_signal);
    std::signal(SIGILL, on_signal);
#endif
}

} // namespace lsd
