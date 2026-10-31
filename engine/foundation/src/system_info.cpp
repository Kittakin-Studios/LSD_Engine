#include "lsd/foundation/system_info.h"
#include "lsd/foundation/log.h"

#include <cstring>
#include <thread>

#if defined(LSD_PLATFORM_WINDOWS)
  #include <windows.h>
  #include <intrin.h>
  #include <psapi.h>
#elif defined(LSD_PLATFORM_LINUX)
  #include <sys/sysinfo.h>
  #if defined(__x86_64__) || defined(__i386__)
    #include <cpuid.h>
  #endif
#endif

namespace lsd {

namespace {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
void cpuid(int out[4], int leaf, int subleaf) {
  #if defined(_MSC_VER)
    __cpuidex(out, leaf, subleaf);
  #else
    unsigned a, b, c, d;
    __cpuid_count(static_cast<unsigned>(leaf), static_cast<unsigned>(subleaf), a, b, c, d);
    out[0] = static_cast<int>(a); out[1] = static_cast<int>(b);
    out[2] = static_cast<int>(c); out[3] = static_cast<int>(d);
  #endif
}

void query_x86(SystemInfo& info) {
    int regs[4] = {};
    cpuid(regs, 0, 0);
    const int max_leaf = regs[0];
    if (max_leaf >= 1) {
        cpuid(regs, 1, 0);
        info.cpu.sse42 = (regs[2] & (1 << 20)) != 0;
    }
    if (max_leaf >= 7) {
        cpuid(regs, 7, 0);
        info.cpu.avx2 = (regs[1] & (1 << 5)) != 0;
        info.cpu.avx512f = (regs[1] & (1 << 16)) != 0;
    }
    // Brand string lives in extended leaves 0x80000002..4.
    cpuid(regs, static_cast<int>(0x80000000u), 0);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        char brand[49] = {};
        for (int i = 0; i < 3; ++i) {
            cpuid(regs, static_cast<int>(0x80000002u) + i, 0);
            std::memcpy(brand + i * 16, regs, 16);
        }
        std::strncpy(info.cpu_brand, brand, sizeof info.cpu_brand - 1);
    }
}
#endif

} // namespace

SystemInfo query_system_info() {
    SystemInfo info{};
    const unsigned cores = std::thread::hardware_concurrency();
    info.logical_cores = cores == 0 ? 1u : cores;

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
    query_x86(info);
#elif defined(__aarch64__) || defined(_M_ARM64)
    info.cpu.neon = true;
#endif

#if defined(LSD_PLATFORM_WINDOWS)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        info.total_ram_bytes = status.ullTotalPhys;
        info.available_ram_bytes = status.ullAvailPhys;
    }
#elif defined(LSD_PLATFORM_LINUX)
    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        info.total_ram_bytes = static_cast<u64>(si.totalram) * si.mem_unit;
        info.available_ram_bytes = static_cast<u64>(si.freeram + si.bufferram) * si.mem_unit;
    }
#endif
    return info;
}

u32 recommended_worker_count(const SystemInfo& info) {
    return info.logical_cores > 1 ? info.logical_cores - 1 : 1;
}

void log_system_info(const SystemInfo& info) {
    LSD_LOG_INFO("system", "CPU: %s", info.cpu_brand[0] ? info.cpu_brand : "unknown");
    LSD_LOG_INFO("system", "Logical cores: %u (job workers: %u)", info.logical_cores, recommended_worker_count(info));
    LSD_LOG_INFO("system", "SIMD: SSE4.2=%d AVX2=%d AVX-512F=%d NEON=%d",
                 info.cpu.sse42, info.cpu.avx2, info.cpu.avx512f, info.cpu.neon);
    LSD_LOG_INFO("system", "RAM: %.1f GiB total, %.1f GiB available",
                 static_cast<double>(info.total_ram_bytes) / static_cast<double>(GiB),
                 static_cast<double>(info.available_ram_bytes) / static_cast<double>(GiB));
}

void process_memory(u64& bytes, u64& peak_bytes) {
    bytes = peak_bytes = 0;
#if defined(LSD_PLATFORM_WINDOWS)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        bytes = counters.WorkingSetSize;
        peak_bytes = counters.PeakWorkingSetSize;
    }
#else
    // shortcut: Linux reads nothing yet; /proc/self/statm when the Linux build is verified
#endif
}

} // namespace lsd
