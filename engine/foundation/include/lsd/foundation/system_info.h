#pragma once
// Hardware report used to size the job system, memory budgets and quality presets.
#include "lsd/foundation/types.h"

namespace lsd {

struct CpuFeatures {
    bool sse42 = false;
    bool avx2 = false;
    bool avx512f = false;
    bool neon = false;
};

struct SystemInfo {
    u32 logical_cores = 1;
    u64 total_ram_bytes = 0;
    u64 available_ram_bytes = 0;
    CpuFeatures cpu{};
    char cpu_brand[64] = {};
};

SystemInfo query_system_info();
// This process's memory in RAM (working set) and its peak; 0 where the platform does not say.
void process_memory(u64& bytes, u64& peak_bytes);

// Worker threads for the job system: logical cores minus one (the main thread), at least one.
u32 recommended_worker_count(const SystemInfo& info);

void log_system_info(const SystemInfo& info);

} // namespace lsd
