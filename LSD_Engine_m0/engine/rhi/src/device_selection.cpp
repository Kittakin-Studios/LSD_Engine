#include "lsd/rhi/rhi.h"

namespace lsd::rhi {

u64 score_adapter(const AdapterInfo& adapter) {
    u64 score = 0;
    switch (adapter.type) {
        case AdapterType::Discrete:   score += 1ull << 62; break;
        case AdapterType::Integrated: score += 1ull << 61; break;
        case AdapterType::Software:   break;
    }
    if (adapter.supports_raytracing)   score += 1ull << 40;
    if (adapter.supports_mesh_shaders) score += 1ull << 39;
    // Memory the GPU can use, in MiB, as the tie-breaker.
    const u64 memory = adapter.dedicated_vram_bytes ? adapter.dedicated_vram_bytes : adapter.shared_memory_bytes / 2;
    score += memory / MiB;
    return score;
}

i32 select_adapter(const AdapterInfo* adapters, u32 count, const DeviceDesc& desc) {
    if (count == 0) return -1;
    if (desc.adapter_override >= 0 && static_cast<u32>(desc.adapter_override) < count) {
        return desc.adapter_override;
    }
    i32 best = 0;
    u64 best_score = score_adapter(adapters[0]);
    for (u32 i = 1; i < count; ++i) {
        const u64 s = score_adapter(adapters[i]);
        if (s > best_score) { best = static_cast<i32>(i); best_score = s; }
    }
    return best;
}

QualityPreset recommend_preset(const AdapterInfo& adapter, u64 system_ram_bytes) {
    // Integrated GPUs share system RAM; treat a fraction of it as their usable memory.
    const u64 gpu_memory = adapter.dedicated_vram_bytes
        ? adapter.dedicated_vram_bytes
        : (adapter.shared_memory_bytes ? adapter.shared_memory_bytes : system_ram_bytes) / 4;

    // Starting thresholds; tuned later against real hardware and exposed in Project Settings.
    if (adapter.type != AdapterType::Discrete || gpu_memory < 3 * GiB || system_ram_bytes < 8 * GiB) {
        return QualityPreset::Low;
    }
    if (gpu_memory < 6 * GiB || system_ram_bytes < 16 * GiB) return QualityPreset::Medium;
    if (gpu_memory < 10 * GiB) return QualityPreset::High;
    return QualityPreset::Epic;
}

const char* to_string(Backend backend) {
    switch (backend) {
        case Backend::Auto:   return "auto";
        case Backend::D3D12:  return "d3d12";
        case Backend::Vulkan: return "vulkan";
        case Backend::OpenGL: return "opengl";
    }
    return "?";
}

const char* to_string(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Low:    return "Low";
        case QualityPreset::Medium: return "Medium";
        case QualityPreset::High:   return "High";
        case QualityPreset::Epic:   return "Epic";
    }
    return "?";
}

} // namespace lsd::rhi
