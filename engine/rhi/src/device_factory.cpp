#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

namespace lsd::rhi {

LightSetup default_lights() {
    LightSetup s;
    s.count = 1;
    GpuLight& sun = s.lights[0];
    sun.type = static_cast<f32>(LightType::Directional);
    const f32 d[3] = {0.5f, 0.35f, -0.8f};  // travelling down and across
    const f32 len = 1.0255f;                 // |d|
    for (int i = 0; i < 3; ++i) sun.direction[i] = d[i] / len;
    sun.color[0] = 2.8f;
    sun.color[1] = 2.69f;
    sun.color[2] = 2.52f;
    return s;
}

#if LSD_RHI_D3D12
Device* create_d3d12_device(const DeviceDesc& desc, Allocator& allocator);
#endif
#if LSD_RHI_VULKAN
Device* create_vulkan_device(const DeviceDesc& desc, Allocator& allocator);
#endif
#if LSD_RHI_OPENGL
Device* create_opengl_device(const DeviceDesc& desc, Allocator& allocator);
#endif

Device* create_device(const DeviceDesc& desc, Allocator& allocator) {
    Backend backend = desc.backend;
    if (backend == Backend::Auto) {
#if LSD_RHI_D3D12
        backend = Backend::D3D12;
#elif LSD_RHI_VULKAN
        backend = Backend::Vulkan;
#else
        backend = Backend::OpenGL;
#endif
    }
    switch (backend) {
#if LSD_RHI_D3D12
        case Backend::D3D12: return create_d3d12_device(desc, allocator);
#endif
#if LSD_RHI_VULKAN
        case Backend::Vulkan: return create_vulkan_device(desc, allocator);
#endif
#if LSD_RHI_OPENGL
        case Backend::OpenGL: return create_opengl_device(desc, allocator);
#endif
        default:
            LSD_LOG_ERROR("rhi", "Backend '%s' is not enabled in this build.", to_string(backend));
            return nullptr;
    }
}

void destroy_device(Device* device, Allocator& allocator) {
    if (!device) return;
    device->~Device();
    allocator.deallocate(device);
}

} // namespace lsd::rhi
