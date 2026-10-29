#pragma once
// Render Hardware Interface: the only graphics API the rest of LSD_Engine talks to.
// Backends (Direct3D 12, Vulkan, OpenGL 4.6) implement Device behind this interface.
#include "lsd/foundation/types.h"

namespace lsd::rhi {

enum class Backend : u8 { Auto, D3D12, Vulkan, OpenGL };
enum class Vendor : u8 { Unknown, Nvidia, Amd, Intel, Other };
enum class AdapterType : u8 { Discrete, Integrated, Software };

struct AdapterInfo {
    char name[128] = {};
    Vendor vendor = Vendor::Unknown;
    AdapterType type = AdapterType::Discrete;
    u32 index = 0;
    u64 dedicated_vram_bytes = 0;   // 0 on integrated GPUs
    u64 shared_memory_bytes = 0;    // system RAM the GPU may use
    bool supports_raytracing = false;
    bool supports_mesh_shaders = false;
};

// Live memory budget reported by the driver (D3D12: QueryVideoMemoryInfo, Vulkan: VK_EXT_memory_budget).
struct MemoryBudget {
    u64 budget_bytes = 0;  // what the OS says we may use right now
    u64 usage_bytes = 0;   // what we currently use
};

enum class QualityPreset : u8 { Low, Medium, High, Epic };

struct DeviceDesc {
    Backend backend = Backend::Auto;
    i32 adapter_override = -1;  // -1 = pick automatically (-gpu=<index> on the command line)
    bool enable_validation = false;
};

class Device {
public:
    virtual ~Device() = default;
    virtual Backend backend() const = 0;
    virtual const AdapterInfo& adapter() const = 0;
    virtual MemoryBudget query_memory_budget() const = 0;
    // Resource creation, command lists and swapchains are added in milestone 1.
};

// Scores an adapter for automatic selection: discrete beats integrated, then VRAM and features.
u64 score_adapter(const AdapterInfo& adapter);

// Returns the index into `adapters` of the best adapter, honouring desc.adapter_override.
i32 select_adapter(const AdapterInfo* adapters, u32 count, const DeviceDesc& desc);

// Picks a starting quality preset from the chosen adapter and system RAM.
QualityPreset recommend_preset(const AdapterInfo& adapter, u64 system_ram_bytes);

const char* to_string(Backend backend);
const char* to_string(QualityPreset preset);

} // namespace lsd::rhi
