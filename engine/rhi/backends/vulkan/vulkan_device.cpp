// Vulkan 1.3 backend: adapter enumeration, VK_EXT_memory_budget, swapchain with dynamic rendering and
// a depth buffer, vertex / index buffers, textures (mipmapped, also shown by ImGui), the sky, lines
// and lit meshes (base colour and its texture, Masked, sun and sky ambient), the test triangle
// (SPIR-V compiled from HLSL by DXC at build time) and ImGui.
// shortcut: no render targets, shadows, skinning, instancing, particles or post pass yet; the scene
// draws straight into the window (the editor then skips the post pass)
#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

// Vulkan's idiom `VkXxxInfo info{VK_STRUCTURE_TYPE_...}` zeroes the rest (pNext, flags...) on purpose.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#if LSD_VULKAN_HAVE_SPIRV
#include "triangle_vs.spv.h"
#include "triangle_ps.spv.h"
#include "mesh_vs.spv.h"
#include "mesh_ps.spv.h"
#include "lines_vs.spv.h"
#include "lines_ps.spv.h"
#include "sky_vs.spv.h"
#include "sky_ps.spv.h"
#endif

namespace lsd::rhi {

namespace {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxAdapters = 16;
constexpr u32 kMaxSwapchainImages = 8;
constexpr u32 kMaxBuffers = 4096;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr u32 kMeshPushBytes = 192;  // mesh.hlsl's Push
constexpr u32 kSkyPushBytes = 160;   // sky.hlsl's Push
constexpr u32 kMaxTextures = 4096;

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    LSD_LOG_ERROR("vulkan", "%s failed (VkResult %d)", what, static_cast<int>(result));
    return false;
}

Vendor vendor_from_id(u32 id) {
    switch (id) {
        case 0x10DE: return Vendor::Nvidia;
        case 0x1002: return Vendor::Amd;
        case 0x8086: return Vendor::Intel;
        default:     return Vendor::Other;
    }
}

bool has_extension(const VkExtensionProperties* extensions, u32 count, const char* name) {
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) LSD_LOG_ERROR("vulkan", "%s", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) LSD_LOG_WARN("vulkan", "%s", data->pMessage);
    return VK_FALSE;
}

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    u32 queue_family = 0;
    bool memory_budget = false;
};

class VulkanDevice final : public Device {
public:
    ~VulkanDevice() override {
        if (device_) vkDeviceWaitIdle(device_);
        if (debug_ui_) {
            for (Texture& t : textures_) {
                if (t.ui_set) ImGui_ImplVulkan_RemoveTexture(t.ui_set);
                t.ui_set = VK_NULL_HANDLE;
            }
            ImGui_ImplVulkan_Shutdown();
        }
        if (device_) {
            destroy_swapchain();
            for (u32 i = 0; i < kFramesInFlight; ++i) {
                vkDestroySemaphore(device_, frames_[i].image_available, nullptr);
                vkDestroyFence(device_, frames_[i].fence, nullptr);
                vkDestroyCommandPool(device_, frames_[i].pool, nullptr);
            }
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
            vkDestroyPipeline(device_, line_pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, line_layout_, nullptr);
            vkDestroyPipeline(device_, sky_pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, sky_layout_, nullptr);
            for (Texture& t : textures_) release_texture(t);
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (texture_pool_) vkDestroyDescriptorPool(device_, texture_pool_, nullptr);
            if (texture_set_layout_) vkDestroyDescriptorSetLayout(device_, texture_set_layout_, nullptr);
            if (upload_pool_) vkDestroyCommandPool(device_, upload_pool_, nullptr);
            for (Buffer& b : buffers_) {
                if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
                if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) {
            if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
            if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            vkDestroyInstance(instance_, nullptr);
        }
        if (sdl_vulkan_loaded_) SDL_Vulkan_UnloadLibrary();
    }

    bool init(const DeviceDesc& desc) {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            LSD_LOG_ERROR("vulkan", "No Vulkan driver found: %s", SDL_GetError());
            return false;
        }
        sdl_vulkan_loaded_ = true;
        volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
        if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
            LSD_LOG_ERROR("vulkan", "Vulkan 1.3 is required; update the GPU driver.");
            return false;
        }
        return create_instance(desc.enable_validation) && pick_adapter(desc) && create_device();
    }

    Backend backend() const override { return Backend::Vulkan; }
    const AdapterInfo& adapter() const override { return info_; }

    MemoryBudget query_memory_budget() const override {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        if (gpu_.memory_budget) props.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(gpu_.handle, &props);
        MemoryBudget out{};
        for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
            if (!(props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            out.budget_bytes += gpu_.memory_budget ? budget.heapBudget[i] : props.memoryProperties.memoryHeaps[i].size;
            out.usage_bytes += gpu_.memory_budget ? budget.heapUsage[i] : 0;
        }
        return out;
    }

    bool create_swapchain(SDL_Window* window, u32 width, u32 height) override {
        if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
            LSD_LOG_ERROR("vulkan", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return false;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu_.handle, gpu_.queue_family, surface_, &present);
        if (!present) {
            LSD_LOG_ERROR("vulkan", "The selected GPU cannot present to this window.");
            return false;
        }
        // Prefer UNORM to match the D3D12 and OpenGL back buffers (no automatic sRGB encode).
        u32 count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_.handle, surface_, &count, nullptr);
        VkSurfaceFormatKHR formats[64];
        count = count < 64 ? count : 64;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_.handle, surface_, &count, formats);
        format_ = formats[0];
        for (u32 i = 0; i < count; ++i) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                format_ = formats[i];
                break;
            }
        }
        return build_swapchain(width, height) && create_triangle_pipeline() && create_scene_pipelines();
    }

    void resize_swapchain(u32 width, u32 height) override {
        if (width == 0 || height == 0) return;
        vkDeviceWaitIdle(device_);
        build_swapchain(width, height);
    }

    void begin_frame(const f32 clear_color[4]) override {
        skip_frame_ = !swapchain_;  // zero-sized (minimized) window
        if (skip_frame_) return;
        Frame& f = frames_[frame_];
        vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            resize_swapchain(extent_.width, extent_.height);
            result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_);
        }
        skip_frame_ = result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR;
        if (skip_frame_) return;
        vkResetFences(device_, 1, &f.fence);
        vkResetCommandPool(device_, f.pool, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &begin);

        transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transition(f.cmd, depth_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views_[image_];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(color.clearValue.color.float32, clear_color, sizeof(f32) * 4);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_view_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = 1.0f;  // as D3D12: 0 near, 1 far, LESS_EQUAL
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent_;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(f.cmd, &rendering);

        // Negative height flips Y so clip space matches D3D12 and OpenGL (+Y up).
        const VkViewport viewport{0.0f, static_cast<f32>(extent_.height), static_cast<f32>(extent_.width), -static_cast<f32>(extent_.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(f.cmd, 0, 1, &viewport);
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    }

    // shortcut: host-visible memory (no staging copy), one allocation per buffer; a staging upload and
    // a sub-allocator (VMA-like) when scenes get large
    BufferHandle create_buffer(BufferUsage usage, const void* data, u64 size) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].buffer) ++slot;
        if (slot == kMaxBuffers || size == 0 || !data) {
            LSD_LOG_ERROR("vulkan", "create_buffer failed: table full (%u) or empty data.", kMaxBuffers);
            return {};
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage == BufferUsage::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        Buffer& b = buffers_[slot];
        if (!check(vkCreateBuffer(device_, &info, nullptr, &b.buffer), "vkCreateBuffer")) return {};
        VkMemoryRequirements need;
        vkGetBufferMemoryRequirements(device_, b.buffer, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped = nullptr;
        if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &b.memory), "vkAllocateMemory (buffer)") ||
            !check(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory") ||
            !check(vkMapMemory(device_, b.memory, 0, size, 0, &mapped), "vkMapMemory")) {
            vkDestroyBuffer(device_, b.buffer, nullptr);
            if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
            b.buffer = VK_NULL_HANDLE;
            b.memory = VK_NULL_HANDLE;
            return {};
        }
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_, b.memory);
        b.size = size;
        return BufferHandle::make(slot, b.generation);
    }

    void destroy_buffer(BufferHandle handle) override {
        Buffer* b = lookup(handle);
        if (!b) return;
        vkDeviceWaitIdle(device_);  // shortcut: stalls; deferred release (per frame in flight) when buffers churn
        vkDestroyBuffer(device_, b->buffer, nullptr);
        vkFreeMemory(device_, b->memory, nullptr);
        b->buffer = VK_NULL_HANDLE;
        b->memory = VK_NULL_HANDLE;
        ++b->generation;
    }
    TextureHandle create_render_target(u32, u32) override { return {}; }
    TextureHandle create_texture(u32 width, u32 height, u32 mip_count, TextureFormat format, const void* const* mips) override {
        if (!texture_set_layout_ || width == 0 || height == 0 || mip_count == 0 || !mips) return {};
        u32 slot = 0;
        while (slot < kMaxTextures && textures_[slot].image) ++slot;
        if (slot == kMaxTextures) {
            LSD_LOG_ERROR("vulkan", "create_texture failed: table full (%u).", kMaxTextures);
            return {};
        }
        mip_count = mip_count < kMaxMips ? mip_count : kMaxMips;
        Texture& t = textures_[slot];
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format == TextureFormat::RGBA8_sRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {width, height, 1};
        image.mipLevels = mip_count;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!check(vkCreateImage(device_, &image, nullptr, &t.image), "vkCreateImage (texture)")) return {};
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(device_, t.image, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &t.memory), "vkAllocateMemory (texture)") ||
            !check(vkBindImageMemory(device_, t.image, t.memory, 0), "vkBindImageMemory (texture)")) {
            release_texture(t);
            return {};
        }
        // The mips through a staging buffer, one copy each (shortcut: waits for the upload; batch them on a transfer queue when loading many)
        VkDeviceSize total = 0;
        for (u32 m = 0; m < mip_count; ++m) total += static_cast<VkDeviceSize>(std::max(1u, width >> m)) * std::max(1u, height >> m) * 4;
        VkBufferCreateInfo staging_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        staging_info.size = total;
        staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool ok = check(vkCreateBuffer(device_, &staging_info, nullptr, &staging), "vkCreateBuffer (staging)");
        if (ok) {
            VkMemoryRequirements sneed;
            vkGetBufferMemoryRequirements(device_, staging, &sneed);
            VkMemoryAllocateInfo salloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            salloc.allocationSize = sneed.size;
            salloc.memoryTypeIndex = memory_type(sneed.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ok = salloc.memoryTypeIndex != UINT32_MAX && check(vkAllocateMemory(device_, &salloc, nullptr, &staging_memory), "vkAllocateMemory (staging)") &&
                 check(vkBindBufferMemory(device_, staging, staging_memory, 0), "vkBindBufferMemory (staging)") &&
                 check(vkMapMemory(device_, staging_memory, 0, total, 0, &mapped), "vkMapMemory (staging)");
        }
        VkBufferImageCopy regions[kMaxMips] = {};
        if (ok) {
            VkDeviceSize at = 0;
            for (u32 m = 0; m < mip_count; ++m) {
                const u32 w = std::max(1u, width >> m), h = std::max(1u, height >> m);
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
                if (mips[m]) std::memcpy(static_cast<u8*>(mapped) + at, mips[m], bytes);
                regions[m].bufferOffset = at;
                regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                regions[m].imageExtent = {w, h, 1};
                at += bytes;
            }
            vkUnmapMemory(device_, staging_memory);
            const VkCommandBuffer cmd = begin_upload();
            ok = cmd != VK_NULL_HANDLE;
            if (ok) {
                transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mip_count);
                vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_count, regions);
                transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mip_count);
                ok = end_upload(cmd);
            }
        }
        if (staging) vkDestroyBuffer(device_, staging, nullptr);
        if (staging_memory) vkFreeMemory(device_, staging_memory, nullptr);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = t.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1};
        ok = ok && check(vkCreateImageView(device_, &view, nullptr, &t.view), "vkCreateImageView (texture)");
        // Its descriptor set for the mesh shader (set 0, binding 0: a combined image sampler).
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = texture_pool_;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &texture_set_layout_;
        ok = ok && check(vkAllocateDescriptorSets(device_, &set_info, &t.set), "vkAllocateDescriptorSets (texture)");
        if (!ok) {
            release_texture(t);
            return {};
        }
        VkDescriptorImageInfo described{sampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = t.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &described;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return TextureHandle::make(slot, t.generation);
    }

    void destroy_texture(TextureHandle handle) override {
        Texture* t = lookup_texture(handle);
        if (!t) return;
        vkDeviceWaitIdle(device_);  // shortcut: stalls; deferred release when textures churn
        if (t->ui_set && debug_ui_) ImGui_ImplVulkan_RemoveTexture(t->ui_set);
        t->ui_set = VK_NULL_HANDLE;
        release_texture(*t);
        ++t->generation;
    }
    void set_render_target(TextureHandle, const f32*) override {}
    u64 debug_ui_texture(TextureHandle handle) override {  // ImGui's descriptor for it (thumbnails, previews), made on first use
        Texture* t = lookup_texture(handle);
        if (!t || !debug_ui_) return 0;
        if (!t->ui_set) t->ui_set = ImGui_ImplVulkan_AddTexture(sampler_, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return reinterpret_cast<u64>(t->ui_set);
    }
    void set_lights(const LightSetup& lights) override {  // the first step's mesh shader takes the sun and the sky / ground ambient
        exposure_ = lights.exposure;
        std::memcpy(sky_, lights.sky, sizeof sky_);
        std::memcpy(ground_, lights.ground, sizeof ground_);
        has_sun_ = false;
        for (u32 i = 0; i < lights.count && i < kMaxLights; ++i) {
            if (lights.lights[i].type >= 0.5f) continue;
            std::memcpy(sun_dir_, lights.lights[i].direction, sizeof sun_dir_);
            std::memcpy(sun_color_, lights.lights[i].color, sizeof sun_color_);
            has_sun_ = true;
            break;
        }
    }
    TextureHandle create_shadow_map(u32) override { return {}; }
    void begin_shadow_pass(TextureHandle) override {}
    void draw_shadow_caster(BufferHandle, BufferHandle, u32, const f32*, const f32*, u32, const ShadowCutout*) override {}
    void draw_shadow_instances(BufferHandle, BufferHandle, u32, const f32*, const f32*, u32) override {}
    void end_shadow_pass() override {}
    void set_shadow_map(TextureHandle, TextureHandle) override {}
    void set_decal_atlas(TextureHandle) override {}
    void set_reflection_atlas(TextureHandle) override {}
    void set_lightmap_atlas(TextureHandle) override {}
    void draw_mesh(const MeshDraw& draw) override {
        if (skip_frame_ || !mesh_pipeline_) return;
        // shortcut: skinned and instanced meshes wait for their pipelines (joint and instance buffers)
        if ((draw.joint_rows && draw.joint_count) || (draw.instance_rows && draw.instance_count)) return;
        const Buffer* vb = lookup(draw.vertices);
        const Buffer* ib = lookup(draw.indices);
        if (!vb || !ib || draw.index_count == 0) return;
        f32 push[kMeshPushBytes / sizeof(f32)] = {};
        std::memcpy(push, draw.mvp, sizeof draw.mvp);
        std::memcpy(push + 16, draw.normal_matrix, sizeof draw.normal_matrix);
        push[19] = draw.unlit || draw.wireframe ? 1.0f : 0.0f;
        push[23] = draw.alpha_cutoff > 0.0f ? draw.alpha_cutoff : 0.0f;  // Masked
        std::memcpy(push + 28, draw.color, sizeof draw.color);
        std::memcpy(push + 32, sun_dir_, sizeof sun_dir_);
        push[35] = exposure_;
        std::memcpy(push + 36, sun_color_, sizeof sun_color_);
        push[39] = has_sun_ ? 1.0f : 0.0f;
        std::memcpy(push + 40, sky_, sizeof(f32) * 3);
        std::memcpy(push + 44, ground_, sizeof(f32) * 3);
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
        vkCmdPushConstants(cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kMeshPushBytes, push);
        const Texture* base = lookup_texture(draw.base_color_texture);
        if (!base) base = lookup_texture(white_);
        if (base) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1, &base->set, 0, nullptr);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &offset);
        vkCmdBindIndexBuffer(cmd, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, draw.index_count, 1, 0, 0, 0);
    }

    void draw_lines(BufferHandle vertices, u32 vertex_count, const f32* mvp) override {
        if (skip_frame_ || !line_pipeline_ || vertex_count == 0) return;
        const Buffer* vb = lookup(vertices);
        if (!vb) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_);
        vkCmdPushConstants(cmd, line_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16, mvp);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &offset);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    }
    void draw_sky(const SkyDraw& sky) override {
        if (skip_frame_ || !sky_pipeline_) return;
        f32 push[kSkyPushBytes / sizeof(f32)];
        std::memcpy(push, sky.inv_view_proj, sizeof sky.inv_view_proj);
        std::memcpy(push + 16, sky.zenith, sizeof sky.zenith);
        std::memcpy(push + 20, sky.horizon, sizeof sky.horizon);
        std::memcpy(push + 24, sky.ground, sizeof sky.ground);
        std::memcpy(push + 28, sky.sun_direction, sizeof sky.sun_direction);
        std::memcpy(push + 32, sky.sun_color, sizeof sky.sun_color);
        std::memcpy(push + 36, sky.camera, sizeof sky.camera);
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        vkCmdPushConstants(cmd, sky_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kSkyPushBytes, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    void draw_ui(const UiDraw&) override {}
    void draw_post(const PostDraw&) override {}
    bool read_render_target(TextureHandle, u8*) override { return false; }
    void draw_particles(const ParticleVertex*, u32, const f32*, TextureHandle, bool) override {}
    MaterialShaderHandle create_material_shader(const char*) override { return {}; }
    void destroy_material_shader(MaterialShaderHandle) override {}

    void set_viewport(f32 x, f32 y, f32 width, f32 height) override {
        if (skip_frame_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        const VkViewport viewport{x, y + height, width, -height, 0.0f, 1.0f};  // flipped like begin_frame
        const VkRect2D scissor{{static_cast<i32>(x), static_cast<i32>(y)}, {static_cast<u32>(width), static_cast<u32>(height)}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void draw_test_triangle(f32 scale, f32 rotation_radians) override {
        if (skip_frame_ || !pipeline_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        const f32 params[3] = {scale, rotation_radians, extent_.height ? static_cast<f32>(extent_.width) / static_cast<f32>(extent_.height) : 1.0f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof params, params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // The screenshot command: the frame is copied out at end_frame and written as a BMP once it is done.
    bool capture_frame(const char* bmp_path) override {
        if (skip_frame_) return false;
        std::snprintf(capture_path_, sizeof capture_path_, "%s", bmp_path);
        return true;
    }

    void end_frame() override {
        if (skip_frame_) return;
        Frame& f = frames_[frame_];
        vkCmdEndRendering(f.cmd);
        VkBuffer capture = VK_NULL_HANDLE;
        VkDeviceMemory capture_memory = VK_NULL_HANDLE;
        if (capture_path_[0]) {  // swapchain image -> a host-visible buffer
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkMemoryRequirements need{};
            if (check(vkCreateBuffer(device_, &info, nullptr, &capture), "vkCreateBuffer (capture)")) {
                vkGetBufferMemoryRequirements(device_, capture, &need);
                VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                alloc.allocationSize = need.size;
                alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &capture_memory), "vkAllocateMemory (capture)") ||
                    !check(vkBindBufferMemory(device_, capture, capture_memory, 0), "vkBindBufferMemory (capture)")) {
                    vkDestroyBuffer(device_, capture, nullptr);
                    if (capture_memory) vkFreeMemory(device_, capture_memory, nullptr);
                    capture = VK_NULL_HANDLE;
                    capture_memory = VK_NULL_HANDLE;
                }
            }
            if (capture) {
                transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {extent_.width, extent_.height, 1};
                vkCmdCopyImageToBuffer(f.cmd, images_[image_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture, 1, &region);
                transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
            }
        }
        if (!capture) {
            transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
        vkEndCommandBuffer(f.cmd);

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = f.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = render_finished_[image_];  // per image: reused only after that image is re-acquired
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmd_info.commandBuffer = f.cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        vkQueueSubmit2(queue_, 1, &submit, f.fence);
        if (capture) {  // wait for this frame, then write the BMP (bottom-up rows, BGR)
            vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
            void* mapped = nullptr;
            bool ok = false;
            if (check(vkMapMemory(device_, capture_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory (capture)")) {
                const u32 w = extent_.width, h = extent_.height, row_bytes = (w * 3 + 3) & ~3u;
                const bool bgra = format_.format == VK_FORMAT_B8G8R8A8_UNORM || format_.format == VK_FORMAT_B8G8R8A8_SRGB;
                u8 header[54] = {'B', 'M'};
                auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<u8>(v >> (8 * i)); };
                put32(2, 54 + row_bytes * h);
                put32(10, 54);
                put32(14, 40);
                put32(18, w);
                put32(22, h);
                header[26] = 1;
                header[28] = 24;
                put32(34, row_bytes * h);
                if (FILE* file = std::fopen(capture_path_, "wb")) {
                    ok = std::fwrite(header, 1, sizeof header, file) == sizeof header;
                    std::vector<u8> row(static_cast<usize>(row_bytes), 0);
                    for (u32 y = 0; ok && y < h; ++y) {
                        const u8* src = static_cast<const u8*>(mapped) + static_cast<usize>(h - 1 - y) * w * 4;
                        for (u32 x = 0; x < w; ++x) {
                            row[x * 3] = src[x * 4 + (bgra ? 0 : 2)];
                            row[x * 3 + 1] = src[x * 4 + 1];
                            row[x * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
                        }
                        ok = std::fwrite(row.data(), 1, row_bytes, file) == row_bytes;
                    }
                    ok = std::fclose(file) == 0 && ok;
                }
                vkUnmapMemory(device_, capture_memory);
            }
            LSD_LOG_INFO("vulkan", ok ? "Saved %s" : "Could not write %s", capture_path_);
            vkDestroyBuffer(device_, capture, nullptr);
            vkFreeMemory(device_, capture_memory, nullptr);
            capture_path_[0] = '\0';
        }

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_[image_];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_;
        const VkResult result = vkQueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) resize_swapchain(extent_.width, extent_.height);
        frame_ = (frame_ + 1) % kFramesInFlight;
    }

    bool init_debug_ui() override {
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = instance_;
        init.PhysicalDevice = gpu_.handle;
        init.Device = device_;
        init.QueueFamily = gpu_.queue_family;
        init.Queue = queue_;
        init.DescriptorPoolSize = 4096;  // backend creates its own pool: fonts plus every texture ImGui shows (thumbnails)
        init.MinImageCount = 2;
        init.ImageCount = image_count_;
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_.format;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = kDepthFormat;  // the pass has a depth buffer
        debug_ui_ = ImGui_ImplVulkan_Init(&init);
        return debug_ui_;
    }

    void debug_ui_new_frame() override { ImGui_ImplVulkan_NewFrame(); }

    void render_debug_ui() override {
        if (!skip_frame_) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frames_[frame_].cmd);
    }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u64 size = 0;
        u32 generation = 0;
    };

    Buffer* lookup(BufferHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxBuffers) return nullptr;
        Buffer& b = buffers_[handle.index()];
        if (!b.buffer || (b.generation & BufferHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &b;
    }

    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;     // the mesh shader's
        VkDescriptorSet ui_set = VK_NULL_HANDLE;  // ImGui's (debug_ui_texture)
        u32 generation = 0;
    };

    Texture* lookup_texture(TextureHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxTextures) return nullptr;
        Texture& t = textures_[handle.index()];
        if (!t.image || (t.generation & TextureHandle::kGenerationMask) != handle.generation()) return nullptr;
        return &t;
    }

    void release_texture(Texture& t) {
        if (t.set) vkFreeDescriptorSets(device_, texture_pool_, 1, &t.set);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        t.set = VK_NULL_HANDLE;
        t.view = VK_NULL_HANDLE;
        t.image = VK_NULL_HANDLE;
        t.memory = VK_NULL_HANDLE;
    }

    // One-off command buffers for uploads, run at once (shortcut: waits for the queue).
    VkCommandBuffer begin_upload() {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = upload_pool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!upload_pool_ || !check(vkAllocateCommandBuffers(device_, &info, &cmd), "vkAllocateCommandBuffers (upload)")) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        return cmd;
    }

    bool end_upload(VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmd_info.commandBuffer = cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        const bool ok = check(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2 (upload)") && check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle (upload)");
        vkFreeCommandBuffers(device_, upload_pool_, 1, &cmd);
        return ok;
    }

    // The sampler, the texture descriptor layout and pool, the upload pool and a white texture (for
    // meshes without one), made once.
    bool create_texture_support() {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        if (!check(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler")) return false;
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &texture_set_layout_), "vkCreateDescriptorSetLayout")) return false;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = kMaxTextures;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (!check(vkCreateDescriptorPool(device_, &pool, nullptr, &texture_pool_), "vkCreateDescriptorPool")) return false;
        VkCommandPoolCreateInfo upload{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        upload.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        upload.queueFamilyIndex = gpu_.queue_family;
        if (!check(vkCreateCommandPool(device_, &upload, nullptr, &upload_pool_), "vkCreateCommandPool (upload)")) return false;
        const u8 white[4] = {255, 255, 255, 255};
        const void* mip = white;
        white_ = create_texture(1, 1, 1, TextureFormat::RGBA8, &mip);
        return white_.valid();
    }

    u32 memory_type(u32 allowed, VkMemoryPropertyFlags wanted) const {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(gpu_.handle, &props);
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((allowed & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        }
        return UINT32_MAX;
    }

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
    };

    bool create_instance(bool validation) {
        u32 sdl_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        const char* extensions[16];
        u32 extension_count = 0;
        for (u32 i = 0; i < sdl_count && extension_count < 15; ++i) extensions[extension_count++] = sdl_extensions[i];

        const char* layers[1];
        u32 layer_count = 0;
        if (validation) {
            u32 available = 0;
            vkEnumerateInstanceLayerProperties(&available, nullptr);
            VkLayerProperties props[64];
            available = available < 64 ? available : 64;
            vkEnumerateInstanceLayerProperties(&available, props);
            for (u32 i = 0; i < available; ++i) {
                if (std::strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
            }
            if (layer_count) {
                extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                LSD_LOG_INFO("vulkan", "Validation layer enabled.");
            } else {
                LSD_LOG_WARN("vulkan", "Validation layer not installed (it ships with the Vulkan SDK).");
            }
        }

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "LSD_Engine";
        app.pEngineName = "LSD_Engine";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = extension_count;
        create.ppEnabledExtensionNames = extensions;
        create.enabledLayerCount = layer_count;
        create.ppEnabledLayerNames = layers;
        if (!check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance")) return false;
        volkLoadInstance(instance_);

        if (layer_count) {
            VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger.pfnUserCallback = debug_callback;
            check(vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
        }
        return true;
    }

    bool pick_adapter(const DeviceDesc& desc) {
        u32 count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        VkPhysicalDevice handles[kMaxAdapters];
        count = count < kMaxAdapters ? count : kMaxAdapters;
        vkEnumeratePhysicalDevices(instance_, &count, handles);

        AdapterInfo infos[kMaxAdapters];
        PhysicalDevice gpus[kMaxAdapters];
        u32 usable = 0;
        for (u32 i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(handles[i], &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;

            u32 family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(handles[i], &family_count, nullptr);
            VkQueueFamilyProperties families[32];
            family_count = family_count < 32 ? family_count : 32;
            vkGetPhysicalDeviceQueueFamilyProperties(handles[i], &family_count, families);
            u32 family = UINT32_MAX;
            for (u32 q = 0; q < family_count && family == UINT32_MAX; ++q) {
                if (families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = q;
            }
            u32 ext_count = 0;
            vkEnumerateDeviceExtensionProperties(handles[i], nullptr, &ext_count, nullptr);
            VkExtensionProperties exts[512];
            ext_count = ext_count < 512 ? ext_count : 512;
            vkEnumerateDeviceExtensionProperties(handles[i], nullptr, &ext_count, exts);
            if (family == UINT32_MAX || !has_extension(exts, ext_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

            AdapterInfo& a = infos[usable];
            SDL_strlcpy(a.name, props.deviceName, sizeof a.name);
            a.index = usable;
            a.vendor = vendor_from_id(props.vendorID);
            switch (props.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: a.type = AdapterType::Integrated; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:            a.type = AdapterType::Software; break;
                default:                                     a.type = AdapterType::Discrete; break;
            }
            VkPhysicalDeviceMemoryProperties mem;
            vkGetPhysicalDeviceMemoryProperties(handles[i], &mem);
            for (u32 h = 0; h < mem.memoryHeapCount; ++h) {
                if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) a.dedicated_vram_bytes += mem.memoryHeaps[h].size;
                else a.shared_memory_bytes += mem.memoryHeaps[h].size;
            }
            if (a.type != AdapterType::Discrete) {  // integrated "device local" memory is system RAM
                a.shared_memory_bytes += a.dedicated_vram_bytes;
                a.dedicated_vram_bytes = 0;
            }
            a.supports_raytracing = has_extension(exts, ext_count, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            a.supports_mesh_shaders = has_extension(exts, ext_count, VK_EXT_MESH_SHADER_EXTENSION_NAME);
            gpus[usable] = {handles[i], family, has_extension(exts, ext_count, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)};
            ++usable;
        }
        if (usable == 0) {
            LSD_LOG_ERROR("vulkan", "No GPU supports Vulkan 1.3 with a graphics queue and swapchain.");
            return false;
        }
        if (desc.adapter_override >= static_cast<i32>(usable)) {
            LSD_LOG_WARN("vulkan", "-gpu=%d is out of range (%u adapters); selecting automatically.", desc.adapter_override, usable);
        }
        const i32 chosen = select_adapter(infos, usable, desc);
        for (u32 i = 0; i < usable; ++i) {
            const AdapterInfo& a = infos[i];
            LSD_LOG_INFO("vulkan", "%s GPU %u: %s (%s, %llu MiB VRAM, %llu MiB shared, RT %s, mesh shaders %s)",
                         static_cast<i32>(i) == chosen ? "*" : " ", i, a.name,
                         a.type == AdapterType::Discrete ? "discrete" : a.type == AdapterType::Integrated ? "integrated" : "software",
                         static_cast<unsigned long long>(a.dedicated_vram_bytes / MiB),
                         static_cast<unsigned long long>(a.shared_memory_bytes / MiB),
                         a.supports_raytracing ? "yes" : "no", a.supports_mesh_shaders ? "yes" : "no");
        }
        info_ = infos[chosen];
        gpu_ = gpus[chosen];
        return true;
    }

    bool create_device() {
        const f32 priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = gpu_.queue_family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.shaderDemoteToHelperInvocation = VK_TRUE;  // core in 1.3: DXC turns HLSL discard into it (Masked materials)
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features13;

        const char* extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &features;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue;
        create.enabledExtensionCount = gpu_.memory_budget ? 2 : 1;
        create.ppEnabledExtensionNames = extensions;
        if (!check(vkCreateDevice(gpu_.handle, &create, nullptr, &device_), "vkCreateDevice")) return false;
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, gpu_.queue_family, 0, &queue_);

        for (Frame& f : frames_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = gpu_.queue_family;
            if (!check(vkCreateCommandPool(device_, &pool, nullptr, &f.pool), "vkCreateCommandPool")) return false;
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = f.pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            if (!check(vkAllocateCommandBuffers(device_, &alloc, &f.cmd), "vkAllocateCommandBuffers")) return false;
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!check(vkCreateFence(device_, &fence, nullptr, &f.fence), "vkCreateFence")) return false;
            if (!check(vkCreateSemaphore(device_, &sem, nullptr, &f.image_available), "vkCreateSemaphore")) return false;
        }
        return true;
    }

    bool build_swapchain(u32 width, u32 height) {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_.handle, surface_, &caps);
        extent_ = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{width, height};
        if (extent_.width == 0 || extent_.height == 0) return true;  // minimized; rebuilt on restore
        u32 min_images = caps.minImageCount + 1;
        if (caps.maxImageCount && min_images > caps.maxImageCount) min_images = caps.maxImageCount;

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = min_images;
        create.imageFormat = format_.format;
        create.imageColorSpace = format_.colorSpace;
        create.imageExtent = extent_;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC: capture_frame
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync, always supported
        create.clipped = VK_TRUE;
        create.oldSwapchain = swapchain_;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        if (!check(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain), "vkCreateSwapchainKHR")) return false;
        destroy_swapchain();
        swapchain_ = swapchain;

        image_count_ = kMaxSwapchainImages;
        vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_);
        if (!create_depth()) return false;
        for (u32 i = 0; i < image_count_; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format_.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!check(vkCreateImageView(device_, &view, nullptr, &views_[i]), "vkCreateImageView")) return false;
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!check(vkCreateSemaphore(device_, &sem, nullptr, &render_finished_[i]), "vkCreateSemaphore")) return false;
        }
        return true;
    }

    void destroy_swapchain() {
        for (u32 i = 0; i < image_count_; ++i) {
            vkDestroyImageView(device_, views_[i], nullptr);
            vkDestroySemaphore(device_, render_finished_[i], nullptr);
            views_[i] = VK_NULL_HANDLE;
            render_finished_[i] = VK_NULL_HANDLE;
        }
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        image_count_ = 0;
        if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
        if (depth_image_) vkDestroyImage(device_, depth_image_, nullptr);
        if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
        depth_image_ = VK_NULL_HANDLE;
        depth_memory_ = VK_NULL_HANDLE;
    }

    // The window-sized depth buffer the scene draws with (made with the swapchain).
    bool create_depth() {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = kDepthFormat;
        image.extent = {extent_.width, extent_.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (!check(vkCreateImage(device_, &image, nullptr, &depth_image_), "vkCreateImage (depth)")) return false;
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(device_, depth_image_, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_), "vkAllocateMemory (depth)") ||
            !check(vkBindImageMemory(device_, depth_image_, depth_memory_, 0), "vkBindImageMemory (depth)")) return false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = depth_image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = kDepthFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        return check(vkCreateImageView(device_, &view, nullptr, &depth_view_), "vkCreateImageView (depth)");
    }

    bool create_triangle_pipeline() {
#if LSD_VULKAN_HAVE_SPIRV
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 3};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (!check(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_), "vkCreatePipelineLayout")) return false;

        VkShaderModule modules[2] = {};
        const u32* code[2] = {kTriangleVsSpirv, kTrianglePsSpirv};
        const usize sizes[2] = {sizeof kTriangleVsSpirv, sizeof kTrianglePsSpirv};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = sizes[i];
            info.pCode = code[i];
            if (!check(vkCreateShaderModule(device_, &info, nullptr, &modules[i]), "vkCreateShaderModule")) return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[0];
        stages[0].pName = "vs_main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[1];
        stages[1].pName = "ps_main";

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format_.format;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineDepthStencilStateCreateInfo no_depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        create.pDepthStencilState = &no_depth;
        create.pNext = &rendering;
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertex_input;
        create.pInputAssemblyState = &input_assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = pipeline_layout_;
        const bool ok = check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline_), "vkCreateGraphicsPipelines");
        vkDestroyShaderModule(device_, modules[0], nullptr);
        vkDestroyShaderModule(device_, modules[1], nullptr);
        return ok;
#else
        LSD_LOG_WARN("vulkan", "Built without SPIR-V shaders (DXC with SPIR-V not found): the test triangle is skipped. "
                               "Install the Vulkan SDK and reconfigure.");
        return true;
#endif
    }

    // A pipeline for the scene pass (colour + depth): vertex layout, topology, depth test, push constants.
    bool make_pipeline(const u32* vs, usize vs_size, const u32* ps, usize ps_size, const VkVertexInputAttributeDescription* attributes, u32 attribute_count,
                       u32 stride, VkPrimitiveTopology topology, u32 push_bytes, VkShaderStageFlags push_stages, VkPipelineLayout& layout_out, VkPipeline& out,
                       bool textured = false, bool depth_test = true) {
        VkPushConstantRange push{push_stages, 0, push_bytes};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (textured) {
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &texture_set_layout_;
        }
        if (!check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_out), "vkCreatePipelineLayout (scene)")) return false;
        VkShaderModule modules[2] = {};
        const u32* code[2] = {vs, ps};
        const usize sizes[2] = {vs_size, ps_size};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = sizes[i];
            info.pCode = code[i];
            if (!check(vkCreateShaderModule(device_, &info, nullptr, &modules[i]), "vkCreateShaderModule (scene)")) return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[0];
        stages[0].pName = "vs_main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[1];
        stages[1].pName = "ps_main";
        VkVertexInputBindingDescription binding{0, stride, VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = attribute_count ? 1 : 0;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = attribute_count;
        vertex_input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = topology;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;  // as D3D12: both sides until imported winding is verified
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format_.format;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        create.pNext = &rendering;
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertex_input;
        create.pInputAssemblyState = &input_assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pDepthStencilState = &depth;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = layout_out;
        const bool ok = check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &out), "vkCreateGraphicsPipelines (scene)");
        vkDestroyShaderModule(device_, modules[0], nullptr);
        vkDestroyShaderModule(device_, modules[1], nullptr);
        return ok;
    }

    bool create_scene_pipelines() {
#if LSD_VULKAN_HAVE_SPIRV
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_.handle, &props);
        if (props.limits.maxPushConstantsSize < kMeshPushBytes) {
            LSD_LOG_WARN("vulkan", "This GPU offers %u bytes of push constants (meshes need %u): meshes are not drawn.", props.limits.maxPushConstantsSize, kMeshPushBytes);
            return true;
        }
        if (!create_texture_support()) return false;
        const VkVertexInputAttributeDescription mesh_attributes[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)},
        };
        const VkVertexInputAttributeDescription line_attributes[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineVertex, color)},
        };
        return make_pipeline(kMeshVsSpirv, sizeof kMeshVsSpirv, kMeshPsSpirv, sizeof kMeshPsSpirv, mesh_attributes, 3, sizeof(MeshVertex),
                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kMeshPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, mesh_layout_, mesh_pipeline_, true) &&
               make_pipeline(kSkyVsSpirv, sizeof kSkyVsSpirv, kSkyPsSpirv, sizeof kSkyPsSpirv, nullptr, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                             kSkyPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sky_layout_, sky_pipeline_, false, false) &&
               make_pipeline(kLinesVsSpirv, sizeof kLinesVsSpirv, kLinesPsSpirv, sizeof kLinesPsSpirv, line_attributes, 2, sizeof(LineVertex),
                             VK_PRIMITIVE_TOPOLOGY_LINE_LIST, sizeof(f32) * 16, VK_SHADER_STAGE_VERTEX_BIT, line_layout_, line_pipeline_);
#else
        return true;
#endif
    }

    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, u32 mips = 1) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, mips, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    AdapterInfo info_{};
    bool sdl_vulkan_loaded_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    PhysicalDevice gpu_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    u32 image_count_ = 0;
    VkImage images_[kMaxSwapchainImages] = {};
    VkImageView views_[kMaxSwapchainImages] = {};
    VkSemaphore render_finished_[kMaxSwapchainImages] = {};
    Frame frames_[kFramesInFlight];
    u32 frame_ = 0;
    u32 image_ = 0;
    bool skip_frame_ = false;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool debug_ui_ = false;
    Buffer buffers_[kMaxBuffers];
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE, line_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE, line_pipeline_ = VK_NULL_HANDLE;
    f32 exposure_ = 1.0f, sky_[4] = {0.30f, 0.36f, 0.45f, 0.0f}, ground_[4] = {0.16f, 0.15f, 0.14f, 0.0f};
    f32 sun_dir_[3] = {0.0f, 0.0f, -1.0f}, sun_color_[3] = {};
    bool has_sun_ = false;
    char capture_path_[260] = "";
    Texture textures_[kMaxTextures];
    TextureHandle white_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool texture_pool_ = VK_NULL_HANDLE;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout sky_layout_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
};

} // namespace

Device* create_vulkan_device(const DeviceDesc& desc, Allocator& allocator) {
    void* memory = allocator.allocate(sizeof(VulkanDevice), alignof(VulkanDevice));
    auto* device = new (memory) VulkanDevice();
    if (!device->init(desc)) {
        destroy_device(device, allocator);
        return nullptr;
    }
    return device;
}

} // namespace lsd::rhi
