#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

namespace lsd::rhi {

#if LSD_RHI_D3D12
Device* create_d3d12_device(const DeviceDesc& desc, Allocator& allocator);
#endif

Device* create_device(const DeviceDesc& desc, Allocator& allocator) {
    Backend backend = desc.backend;
    if (backend == Backend::Auto) {
#if LSD_RHI_D3D12
        backend = Backend::D3D12;
#else
        backend = Backend::Vulkan;
#endif
    }
    switch (backend) {
#if LSD_RHI_D3D12
        case Backend::D3D12: return create_d3d12_device(desc, allocator);
#endif
        default:
            LSD_LOG_ERROR("rhi", "Backend '%s' is not implemented yet.", to_string(backend));
            return nullptr;
    }
}

void destroy_device(Device* device, Allocator& allocator) {
    if (!device) return;
    device->~Device();
    allocator.deallocate(device);
}

} // namespace lsd::rhi
