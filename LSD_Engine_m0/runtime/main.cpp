// LSD_Engine runtime entry point.
// Milestone 0: prints the hardware report the engine will use to size jobs, memory and quality.
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <cstring>
#include <cstdlib>

int main(int argc, char** argv) {
    lsd::rhi::DeviceDesc desc{};
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-gpu=", 5) == 0) desc.adapter_override = std::atoi(argv[i] + 5);
        else if (std::strcmp(argv[i], "-rhi=d3d12") == 0) desc.backend = lsd::rhi::Backend::D3D12;
        else if (std::strcmp(argv[i], "-rhi=vulkan") == 0) desc.backend = lsd::rhi::Backend::Vulkan;
        else if (std::strcmp(argv[i], "-rhi=opengl") == 0) desc.backend = lsd::rhi::Backend::OpenGL;
    }

    LSD_LOG_INFO("engine", "LSD_Engine 0.1.0");
    const lsd::SystemInfo info = lsd::query_system_info();
    lsd::log_system_info(info);
    LSD_LOG_INFO("engine", "Requested graphics backend: %s", lsd::rhi::to_string(desc.backend));
    LSD_LOG_INFO("engine", "Graphics backends are implemented in milestone 1.");
    return 0;
}
