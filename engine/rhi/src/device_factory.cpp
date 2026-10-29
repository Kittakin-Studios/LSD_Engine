#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

namespace lsd::rhi {

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
