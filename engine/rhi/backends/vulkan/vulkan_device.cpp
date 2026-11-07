// Vulkan 1.3 backend: adapter enumeration, VK_EXT_memory_budget, swapchain with dynamic rendering and
// a depth buffer, vertex / index buffers, textures (mipmapped, also shown by ImGui), the sky, lines
// and lit meshes (base colour and its texture, Masked, sun and sky ambient), the test triangle
// (SPIR-V compiled from HLSL by DXC at build time) and ImGui.
// Skinned and instanced meshes read their joint / copy matrices from a per-frame ring buffer; the sun
// and the spot / point lights cast shadows (depth-only passes into shadow atlases, compare sampler);
// game UI and particles stream their vertices through a per-frame buffer. Render targets (colour +
// depth) take thumbnails, previews and the scene for the post pass, which runs D3D12's shared
// shaders/post.hlsl compiled to SPIR-V.
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
#include "mesh_skinned_vs.spv.h"
#include "mesh_skinned_ps.spv.h"
#include "mesh_instanced_vs.spv.h"
#include "mesh_instanced_ps.spv.h"
#include "shadow_vs.spv.h"
#include "shadow_ps.spv.h"
#include "shadow_skinned_vs.spv.h"
#include "shadow_instanced_vs.spv.h"
#include "ui_vs.spv.h"
#include "ui_ps.spv.h"
#include "particle_vs.spv.h"
#include "particle_ps.spv.h"
#include "post_full.spv.h"
#include "post_prefilter.spv.h"
#include "post_down.spv.h"
#include "post_up.spv.h"
#include "post_composite.spv.h"
#include "post_fxaa.spv.h"
#include "post_ssr.spv.h"
#include "post_ao.spv.h"
#include "post_ao_apply.spv.h"
#include "post_taa.spv.h"
#include "post_dof.spv.h"
#include "post_motion.spv.h"
#include "post_meter.spv.h"
#include "post_expose.spv.h"
#include "post_outline.spv.h"
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
constexpr VkDeviceSize kRingBytes = 8u << 20;      // per frame in flight: joints and instance matrices
constexpr VkDeviceSize kRowsWindow = 64u << 10;    // what one draw may read from it (the descriptor's range)
constexpr u32 kInstancesPerDraw = static_cast<u32>(kRowsWindow / 48);  // 3 float4 rows per copy
constexpr u32 kShadowPushBytes = 80;  // shadow.hlsl's Push
constexpr u32 kUiPushBytes = 16;      // ui.hlsl's Push
constexpr u32 kParticlePushBytes = 80;  // particle.hlsl's Push
constexpr VkDeviceSize kStreamBytes = kParticleRingBytes;  // per frame in flight: UI and particle vertices
enum class Blend { Opaque, Premultiplied, Alpha, Additive };  // make_pipeline's colour blending
constexpr u32 kPostPushBytes = 192;  // post.hlsl's Post constants (48 floats)
enum PostPass : u32 { kPrefilter, kDown, kUp, kComposite, kFxaa, kSsr, kAo, kAoApply, kTaa, kDof, kMotion, kMeter, kExpose, kOutline, kPostPasses };
constexpr u32 kPostSetsPerFrame = 512;  // post passes' descriptor sets a frame (about 16 a view)

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
private:
    struct Texture;  // defined below; passes take it by reference

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
            vkDestroyPipeline(device_, skinned_pipeline_, nullptr);
            vkDestroyPipeline(device_, instanced_pipeline_, nullptr);
            for (VkPipeline shadow : shadow_pipelines_) vkDestroyPipeline(device_, shadow, nullptr);
            vkDestroyPipeline(device_, ui_pipeline_, nullptr);
            vkDestroyPipeline(device_, particle_pipelines_[0], nullptr);
            vkDestroyPipeline(device_, particle_pipelines_[1], nullptr);
            vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
            vkDestroyPipelineLayout(device_, particle_layout_, nullptr);
            for (VkPipeline post : post_pipelines_) vkDestroyPipeline(device_, post, nullptr);
            vkDestroyPipelineLayout(device_, post_layout_, nullptr);
            for (Frame& f : frames_) {
                if (f.post_pool) vkDestroyDescriptorPool(device_, f.post_pool, nullptr);
            }
            if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
            if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
            if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
            for (u32 i = 0; i < shadow_set_count_; ++i) vkFreeDescriptorSets(device_, texture_pool_, 1, &shadow_sets_[i].set);
            if (shadow_set_layout_) vkDestroyDescriptorSetLayout(device_, shadow_set_layout_, nullptr);
            for (Frame& f : frames_) {
                if (f.ring) vkDestroyBuffer(device_, f.ring, nullptr);
                if (f.ring_memory) vkFreeMemory(device_, f.ring_memory, nullptr);
                if (f.stream) vkDestroyBuffer(device_, f.stream, nullptr);
                if (f.stream_memory) vkFreeMemory(device_, f.stream_memory, nullptr);
            }
            if (rows_pool_) vkDestroyDescriptorPool(device_, rows_pool_, nullptr);
            if (rows_set_layout_) vkDestroyDescriptorSetLayout(device_, rows_set_layout_, nullptr);
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
        f.ring_used = 0;  // the GPU is done with this frame's ring and stream
        f.stream_used = 0;
        if (f.post_pool) vkResetDescriptorPool(device_, f.post_pool, 0);
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
        scene_in_ring_ = false;
        shadow_pass_ = nullptr;
        target_ = nullptr;
        sun_shadow_ = {};
        local_shadow_ = {};
        begin_main_pass(clear_color);
    }

    // The window pass (colour + depth). clear_color null: carries on after a shadow pass (load what is there).
    void begin_main_pass(const f32* clear_color) {
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views_[image_];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear_color) std::memcpy(color.clearValue.color.float32, clear_color, sizeof(f32) * 4);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_view_;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // kept: a shadow pass may split the frame's pass
        depth.clearValue.depthStencil.depth = 1.0f;  // as D3D12: 0 near, 1 far, LESS_EQUAL
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent_;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
        in_pass_ = true;
        set_viewport(0.0f, 0.0f, static_cast<f32>(extent_.width), static_cast<f32>(extent_.height));
    }

    // Ends the pass being recorded (the window's, a render target's, a shadow map's); a render
    // target's colour goes back to shader-readable.
    void end_pass() {
        if (!in_pass_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdEndRendering(cmd);
        in_pass_ = false;
        if (target_) {
            transition(cmd, target_->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            target_ = nullptr;
        }
    }

    // Draws into a render target: cleared to clear_color (null: carries on with what it holds).
    void begin_target_pass(Texture& t, const f32* clear_color) {
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        transition(cmd, t.image, clear_color ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (clear_color || t.depth_readable) {  // a cleared depth starts from nothing; a read one goes back to being drawn into
            transition(cmd, t.depth_image, clear_color ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            t.depth_readable = false;
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear_color) std::memcpy(color.clearValue.color.float32, clear_color, sizeof(f32) * 4);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = t.depth_view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {t.width, t.height};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
        in_pass_ = true;
        target_ = &t;
        set_viewport(0.0f, 0.0f, static_cast<f32>(t.width), static_cast<f32>(t.height));
    }

    // A device-local image and its view (render targets' colour and depth).
    bool make_image(u32 width, u32 height, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        if (!check(vkCreateImage(device_, &info, nullptr, &image), "vkCreateImage (render target)")) return false;
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(device_, image, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &memory), "vkAllocateMemory (render target)") ||
            !check(vkBindImageMemory(device_, image, memory, 0), "vkBindImageMemory (render target)")) return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0, 1, 0, 1};
        return check(vkCreateImageView(device_, &view_info, nullptr, &view), "vkCreateImageView (render target)");
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
    // Offscreen colour (the window's format) with its own depth buffer; the colour is kept
    // shader-readable between passes, the depth readable by the post pass.
    TextureHandle create_render_target(u32 width, u32 height) override {
        if (!texture_set_layout_ || width == 0 || height == 0) return {};
        u32 slot = 0;
        while (slot < kMaxTextures && textures_[slot].image) ++slot;
        if (slot == kMaxTextures) {
            LSD_LOG_ERROR("vulkan", "create_render_target failed: table full (%u).", kMaxTextures);
            return {};
        }
        Texture& t = textures_[slot];
        bool ok = make_image(width, height, format_.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, t.image, t.memory, t.view) &&
                  make_image(width, height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                             t.depth_image, t.depth_memory, t.depth_view);
        if (ok) {
            const VkCommandBuffer cmd = begin_upload();
            ok = cmd != VK_NULL_HANDLE;
            if (ok) {
                transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                transition(cmd, t.depth_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                ok = end_upload(cmd);
            }
        }
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = texture_pool_;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &texture_set_layout_;
        ok = ok && check(vkAllocateDescriptorSets(device_, &set_info, &t.set), "vkAllocateDescriptorSets (render target)");
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
        t.width = width;
        t.height = height;
        t.depth_readable = true;
        return TextureHandle::make(slot, t.generation);
    }
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
        if (t->shadow_size) {  // its view leaves the cached shadow sets (the GPU is idle here)
            for (u32 i = 0; i < shadow_set_count_; ++i) vkFreeDescriptorSets(device_, texture_pool_, 1, &shadow_sets_[i].set);
            shadow_set_count_ = 0;
        }
        release_texture(*t);
        ++t->generation;
    }
    void set_render_target(TextureHandle handle, const f32 clear_color[4]) override {
        if (skip_frame_ || shadow_pass_) return;
        Texture* t = lookup_texture(handle);
        end_pass();
        if (t && t->depth_image) begin_target_pass(*t, clear_color);
        else begin_main_pass(nullptr);  // back to the window, not cleared again
    }
    u64 debug_ui_texture(TextureHandle handle) override {  // ImGui's descriptor for it (thumbnails, previews), made on first use
        Texture* t = lookup_texture(handle);
        if (!t || !debug_ui_) return 0;
        if (!t->ui_set) t->ui_set = ImGui_ImplVulkan_AddTexture(sampler_, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return reinterpret_cast<u64>(t->ui_set);
    }
    void set_lights(const LightSetup& lights) override {  // mesh.hlsl's Scene: LightSetup up to the local shadow tiles, and its output
        std::memcpy(scene_.prefix, &lights, sizeof scene_.prefix);
        std::memcpy(scene_.output, lights.output, sizeof scene_.output);
        scene_in_ring_ = false;
    }

    // A square depth texture, kept in the shader-read layout between shadow passes (cleared to far at first).
    TextureHandle create_shadow_map(u32 size) override {
        if (!texture_set_layout_ || !shadow_sampler_ || size == 0) return {};
        u32 slot = 0;
        while (slot < kMaxTextures && textures_[slot].image) ++slot;
        if (slot == kMaxTextures) return {};
        Texture& t = textures_[slot];
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = kDepthFormat;
        image.extent = {size, size, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!check(vkCreateImage(device_, &image, nullptr, &t.image), "vkCreateImage (shadow map)")) return {};
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(device_, t.image, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        bool ok = alloc.memoryTypeIndex != UINT32_MAX && check(vkAllocateMemory(device_, &alloc, nullptr, &t.memory), "vkAllocateMemory (shadow map)") &&
                  check(vkBindImageMemory(device_, t.image, t.memory, 0), "vkBindImageMemory (shadow map)");
        if (ok) {
            const VkCommandBuffer cmd = begin_upload();
            ok = cmd != VK_NULL_HANDLE;
            if (ok) {
                transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                const VkClearDepthStencilValue far{1.0f, 0};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                vkCmdClearDepthStencilImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);
                transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                ok = end_upload(cmd);
            }
        }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = t.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = kDepthFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        ok = ok && check(vkCreateImageView(device_, &view, nullptr, &t.view), "vkCreateImageView (shadow map)");
        if (!ok) {
            release_texture(t);
            return {};
        }
        t.shadow_size = size;
        return TextureHandle::make(slot, t.generation);
    }

    // Leaves the window pass, draws depth into the shadow map (set_viewport picks its tiles); end_shadow_pass returns.
    void begin_shadow_pass(TextureHandle handle) override {
        Texture* t = lookup_texture(handle);
        if (skip_frame_ || shadow_pass_ || !t || !t->shadow_size) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        resume_target_ = target_;  // end_shadow_pass carries on drawing into it
        end_pass();
        transition(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = t->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {t->shadow_size, t->shadow_size};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
        in_pass_ = true;
        shadow_pass_ = t;
        const f32 size = static_cast<f32>(t->shadow_size);
        set_viewport(0.0f, 0.0f, size, size);
    }

    void draw_shadow_caster(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 light_mvp[16],
                            const f32* joint_rows, u32 joint_count, const ShadowCutout* cutout) override {
        const bool skinned = joint_rows && joint_count;
        const Texture* masked = !skinned && cutout ? lookup_texture(cutout->texture) : nullptr;  // shortcut: skinned casters cast solid
        draw_shadow(vertices, indices, index_count, light_mvp, skinned ? 1 : masked ? 3 : 0, joint_rows, skinned ? std::min(joint_count, kMaxJoints) : 0, masked ? masked->set : VK_NULL_HANDLE,
                    masked ? cutout->cutoff : 0.0f, masked ? cutout->alpha : 1.0f);
    }

    void draw_shadow_instances(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 view_proj[16], const f32* rows, u32 count) override {
        if (rows && count) draw_shadow(vertices, indices, index_count, view_proj, 2, rows, count, VK_NULL_HANDLE, 0.0f, 1.0f);
    }

    void end_shadow_pass() override {
        if (skip_frame_ || !shadow_pass_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        end_pass();
        transition(cmd, shadow_pass_->image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        shadow_pass_ = nullptr;
        if (resume_target_) begin_target_pass(*resume_target_, nullptr);
        else begin_main_pass(nullptr);
        resume_target_ = nullptr;
    }

    void set_shadow_map(TextureHandle sun, TextureHandle local) override {
        sun_shadow_ = sun;
        local_shadow_ = local;
    }
    void set_decal_atlas(TextureHandle) override {}
    void set_reflection_atlas(TextureHandle) override {}
    void set_lightmap_atlas(TextureHandle) override {}
    void draw_mesh(const MeshDraw& draw) override {
        if (skip_frame_ || !mesh_pipeline_) return;
        const bool skinned = draw.joint_rows && draw.joint_count;
        const bool instanced = !skinned && draw.instance_rows && draw.instance_count;
        if ((skinned && !skinned_pipeline_) || (instanced && !instanced_pipeline_)) return;
        const Buffer* vb = lookup(draw.vertices);
        const Buffer* ib = lookup(draw.indices);
        if (!vb || !ib || draw.index_count == 0) return;
        Frame& f = frames_[frame_];
        if (!scene_in_ring_) {  // this view's Scene block, once per set_lights
            if (!ring_write(f, &scene_, sizeof scene_, scene_at_)) return;
            scene_in_ring_ = true;
        }
        f32 push[kMeshPushBytes / sizeof(f32)] = {};
        std::memcpy(push, draw.mvp, sizeof draw.mvp);
        std::memcpy(push + 16, draw.world, sizeof draw.world);
        std::memcpy(push + 28, draw.normal_matrix, sizeof draw.normal_matrix);
        push[31] = draw.unlit || draw.wireframe ? 1.0f : 0.0f;
        push[35] = draw.alpha_cutoff > 0.0f ? draw.alpha_cutoff : 0.0f;  // Masked
        push[39] = draw.roughness;
        std::memcpy(push + 40, draw.color, sizeof draw.color);
        std::memcpy(push + 44, draw.camera_position, sizeof draw.camera_position);
        const VkCommandBuffer cmd = f.cmd;
        const VkPipelineLayout layout = mesh_layout_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned ? skinned_pipeline_ : instanced ? instanced_pipeline_ : mesh_pipeline_);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kMeshPushBytes, push);
        const Texture* base = lookup_texture(draw.base_color_texture);
        if (!base) base = lookup_texture(white_);
        if (base) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &base->set, 0, nullptr);
        if (const VkDescriptorSet shadows = shadow_set(sun_shadow_, local_shadow_))
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &shadows, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &f.rows_set, 1, &scene_at_);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &offset);
        vkCmdBindIndexBuffer(cmd, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
        if (!skinned && !instanced) {
            vkCmdDrawIndexed(cmd, draw.index_count, 1, 0, 0, 0);
            return;
        }
        // Joints (all at once, at most kMaxJoints) or the copies in batches the descriptor's window holds.
        const u32 total = skinned ? std::min(draw.joint_count, kMaxJoints) : draw.instance_count;
        const f32* rows = skinned ? draw.joint_rows : draw.instance_rows;
        for (u32 first = 0; first < total;) {
            const u32 count = skinned ? total : std::min(total - first, kInstancesPerDraw);
            u32 at = 0;
            if (!ring_write(f, rows + static_cast<usize>(first) * 12, static_cast<VkDeviceSize>(count) * 48, at)) return;  // full this frame: the rest waits
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &f.rows_set, 1, &at);
            vkCmdDrawIndexed(cmd, draw.index_count, skinned ? 1 : count, 0, 0, 0);
            first += count;
        }
    }

    // Set 3: the sun's and the local lights' atlases (the far 1x1 map for a missing one), one set per pair, kept.
    VkDescriptorSet shadow_set(TextureHandle sun, TextureHandle local) {
        const Texture* maps[2] = {lookup_texture(sun), lookup_texture(local)};
        for (const Texture*& m : maps) {
            if (!m || !m->shadow_size) m = lookup_texture(no_shadow_);
            if (!m) return VK_NULL_HANDLE;
        }
        for (u32 i = 0; i < shadow_set_count_; ++i) {
            if (shadow_sets_[i].sun == maps[0]->view && shadow_sets_[i].local == maps[1]->view) return shadow_sets_[i].set;
        }
        if (shadow_set_count_ == kMaxShadowSets) {  // atlases re-made many times: start over (none is in use once idle)
            vkDeviceWaitIdle(device_);
            for (u32 i = 0; i < shadow_set_count_; ++i) vkFreeDescriptorSets(device_, texture_pool_, 1, &shadow_sets_[i].set);
            shadow_set_count_ = 0;
        }
        ShadowSet& entry = shadow_sets_[shadow_set_count_];
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = texture_pool_;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &shadow_set_layout_;
        if (!check(vkAllocateDescriptorSets(device_, &info, &entry.set), "vkAllocateDescriptorSets (shadows)")) return VK_NULL_HANDLE;
        const VkDescriptorImageInfo images[2] = {{shadow_sampler_, maps[0]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {shadow_sampler_, maps[1]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = entry.set;
        write.descriptorCount = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = images;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        entry.sun = maps[0]->view;
        entry.local = maps[1]->view;
        ++shadow_set_count_;
        return entry.set;
    }

    // One caster: kind 0 static, 1 skinned (rows = joints), 2 instanced (rows = copies, mvp = the light's view-projection), 3 Masked.
    void draw_shadow(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32* mvp, int kind, const f32* rows, u32 row_count,
                     VkDescriptorSet masked, f32 cutoff, f32 alpha) {
        if (skip_frame_ || !shadow_pass_ || !shadow_pipelines_[kind] || index_count == 0) return;
        const Buffer* vb = lookup(vertices);
        const Buffer* ib = lookup(indices);
        if (!vb || !ib) return;
        Frame& f = frames_[frame_];
        const VkCommandBuffer cmd = f.cmd;
        f32 push[kShadowPushBytes / sizeof(f32)] = {};
        std::memcpy(push, mvp, sizeof(f32) * 16);
        push[16] = cutoff;
        push[17] = alpha;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipelines_[kind]);
        vkCmdPushConstants(cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kShadowPushBytes, push);
        if (masked) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1, &masked, 0, nullptr);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &offset);
        vkCmdBindIndexBuffer(cmd, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
        if (kind == 0 || kind == 3) {
            vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
            return;
        }
        for (u32 first = 0; first < row_count;) {
            const u32 count = kind == 1 ? row_count : std::min(row_count - first, kInstancesPerDraw);
            u32 at = 0;
            if (!ring_write(f, rows + static_cast<usize>(first) * 12, static_cast<VkDeviceSize>(count) * 48, at)) return;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 1, 1, &f.rows_set, 1, &at);
            vkCmdDrawIndexed(cmd, index_count, kind == 1 ? 1 : count, 0, 0, 0);
            first += count;
        }
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
        push[39] = scene_.output[0];  // into a scene target: range-compressed
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        vkCmdPushConstants(cmd, sky_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kSkyPushBytes, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    void draw_ui(const UiDraw& d) override {
        if (skip_frame_ || shadow_pass_ || !ui_pipeline_ || !d.vertex_count || !d.index_count || d.viewport[2] <= 0.0f || d.viewport[3] <= 0.0f) return;
        Frame& f = frames_[frame_];
        VkDeviceSize vat = 0, iat = 0;
        if (!stream_write(f, d.vertices, static_cast<VkDeviceSize>(d.vertex_count) * sizeof(UiVertex), vat) ||
            !stream_write(f, d.indices, static_cast<VkDeviceSize>(d.index_count) * 4, iat)) return;
        const f32 push[4] = {d.translation[0], d.translation[1], 2.0f / d.viewport[2], 2.0f / d.viewport[3]};
        const Texture* t = lookup_texture(d.texture);
        if (!t || t->shadow_size) t = lookup_texture(white_);
        if (!t) return;
        const VkCommandBuffer cmd = f.cmd;
        const f32 saved[4] = {viewport_[0], viewport_[1], viewport_[2], viewport_[3]};
        set_viewport(d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3]);
        if (d.scissor[2] > 0) {  // inside the viewport; Vulkan wants a non-negative offset
            const i32 x = std::max(0, static_cast<i32>(d.viewport[0]) + d.scissor[0]), y = std::max(0, static_cast<i32>(d.viewport[1]) + d.scissor[1]);
            const VkRect2D scissor{{x, y}, {static_cast<u32>(std::max(0, d.scissor[2])), static_cast<u32>(std::max(0, d.scissor[3]))}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        vkCmdPushConstants(cmd, ui_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kUiPushBytes, push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout_, 0, 1, &t->set, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.stream, &vat);
        vkCmdBindIndexBuffer(cmd, f.stream, iat, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, d.index_count, 1, 0, 0, 0);
        set_viewport(saved[0], saved[1], saved[2], saved[3]);  // back to the caller's rectangle
    }
    // Post-processing (Unreal's post-process stack), as D3D12's draw_post: the same passes from
    // shaders/post.hlsl, into a chain of targets per scene size.
    void draw_post(const PostDraw& p) override {
        Texture* scene = lookup_texture(p.scene);
        if (skip_frame_ || shadow_pass_ || !scene || !scene->depth_image || !post_pipelines_[kComposite]) return;
        PostChain& c = post_chain(scene->width, scene->height);
        auto size = [&](TextureHandle t, f32* out) {
            const Texture* target = lookup_texture(t);
            out[0] = target && target->width ? 1.0f / static_cast<f32>(target->width) : 0.0f;
            out[1] = target && target->height ? 1.0f / static_cast<f32>(target->height) : 0.0f;
        };
        const bool depth_effects = p.proj[0] != 0.0f;
        const bool ao = depth_effects && (p.ao_intensity > 0.0f || p.contact_shadow[3] > 0.0f);
        const bool dof = depth_effects && p.dof_focal_distance > 0.0f;
        const bool motion = depth_effects && p.motion_blur > 0.0f && !p.taa_reset;
        const bool taa = depth_effects && p.taa;
        const bool ssr = depth_effects && p.ssr_intensity > 0.0f;
        if (!taa) c.history_valid = false;
        f32 k[48] = {};  // the constants every pass reads (post.hlsl's Post)
        for (int i = 0; i < 4; ++i) k[16 + i] = p.proj[i];
        k[20] = p.ao_radius > 0.01f ? p.ao_radius : 0.01f;
        k[21] = p.ao_intensity;
        k[22] = p.ssr_intensity;
        k[23] = p.ssr_max_distance > 0.5f ? p.ssr_max_distance : 0.5f;
        k[24] = p.dof_focal_distance;
        k[25] = p.dof_focal_region;
        k[26] = p.dof_max_blur;
        k[27] = p.dof_transition;
        k[28] = 0.1f;  // TAA: how much of this frame goes in
        k[29] = (p.taa_reset || !c.history_valid) ? 1.0f : 0.0f;
        k[30] = p.motion_blur;
        k[31] = p.motion_blur_max;
        for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) k[32 + r * 4 + col] = p.reproject[col * 4 + r];  // rows
        }
        size(p.scene, k);
        end_pass();  // the scene's colour (and depth below) become readable
        if ((ao || dof || taa || ssr || motion) && !scene->depth_readable) {
            const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            transition(frames_[frame_].cmd, scene->depth_image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tests,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            scene->depth_readable = true;
        }
        TextureHandle src = p.scene;
        if (ssr) {  // first: it reads the scene's alpha (reflectivity)
            post_pass(kSsr, c.ssr, nullptr, p.scene, {}, p.scene, k);
            src = c.ssr;
        }
        if (ao) {
            f32 ka[48];  // the AO pass reads no reprojection: its first row carries the contact shadow ray
            std::memcpy(ka, k, sizeof ka);
            std::memcpy(ka + 32, p.contact_shadow, sizeof p.contact_shadow);
            post_pass(kAo, c.ao, nullptr, {}, {}, p.scene, ka);
            post_pass(kAoApply, c.lit, nullptr, src, c.ao, p.scene, k);
            src = c.lit;
        }
        if (taa) {
            const TextureHandle out = c.taa[c.taa_index], history = c.taa[c.taa_index ^ 1u];
            post_pass(kTaa, out, nullptr, src, history, p.scene, k);
            c.taa_index ^= 1u;
            c.history_valid = true;
            src = out;
        }
        if (dof) {
            post_pass(kDof, c.dof, nullptr, src, {}, p.scene, k);
            src = c.dof;
        }
        if (motion) {
            post_pass(kMotion, c.motion, nullptr, src, {}, p.scene, k);
            src = c.motion;
        }
        if (p.auto_exposure) {  // eye adaptation: meter into 1 x 1 (eased from the last frame's), then apply it
            f32 e[48] = {};
            e[4] = p.exposure_min_ev;
            e[5] = p.exposure_max_ev;
            e[6] = p.exposure_compensation;
            e[7] = c.exposure_valid && p.delta_seconds > 0.0f ? 0.0f : 1.0f;  // 1: jump to the target
            e[8] = p.delta_seconds * p.exposure_speed_up;
            e[9] = p.delta_seconds * p.exposure_speed_down;
            const TextureHandle now = c.exposure[c.exposure_index], last = c.exposure[c.exposure_index ^ 1u];
            post_pass(kMeter, now, nullptr, src, last, {}, e);
            post_pass(kExpose, c.exposed, nullptr, src, now, {}, e);
            c.exposure_index ^= 1u;
            c.exposure_valid = true;
            src = c.exposed;
        } else {
            c.exposure_valid = false;
        }
        const bool bloom = p.bloom_intensity > 0.0f;
        if (bloom) {
            k[4] = p.bloom_threshold;
            size(src, k);
            post_pass(kPrefilter, c.down[0], nullptr, src, {}, {}, k);
            for (u32 i = 1; i < 5; ++i) {
                size(c.down[i - 1], k);
                post_pass(kDown, c.down[i], nullptr, c.down[i - 1], {}, {}, k);
            }
            for (i32 i = 3; i >= 0; --i) {  // up[i] = down[i] + blur(up[i + 1]); the lowest uses down[4]
                const TextureHandle below = i == 3 ? c.down[4] : c.up[i + 1];
                size(c.down[i], k);
                size(below, k + 2);
                post_pass(kUp, c.up[i], nullptr, c.down[i], below, {}, k);
            }
        }
        f32 g[48] = {};
        g[5] = bloom ? p.bloom_intensity : 0.0f;
        g[8] = p.tint[0];
        g[9] = p.tint[1];
        g[10] = p.tint[2];
        g[11] = p.tonemap ? 1.0f : 0.0f;
        g[12] = p.saturation;
        g[13] = p.contrast;
        g[14] = p.vignette;
        g[15] = p.color_lut.valid() ? p.lut_intensity : 0.0f;
        const bool fxaa = p.fxaa && !taa;
        post_pass(kComposite, fxaa ? c.ldr : p.output, p.viewport, src, bloom ? c.up[0] : TextureHandle{}, p.color_lut, g);
        if (fxaa) {
            f32 f[48] = {};
            size(c.ldr, f);
            post_pass(kFxaa, p.output, p.viewport, c.ldr, {}, {}, f);
        }
        set_render_target({}, nullptr);
        set_viewport(p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3]);
    }

    // The selection outline: the selected meshes as a white mask, its edge painted over the viewport.
    void draw_outline(const MeshDraw* draws, u32 count, const f32 color[4], const f32 viewport[4]) override {
        const u32 w = static_cast<u32>(viewport[2]), h = static_cast<u32>(viewport[3]);
        if (skip_frame_ || shadow_pass_ || !count || !w || !h || !post_pipelines_[kOutline]) return;
        if (outline_size_[0] != w || outline_size_[1] != h) {  // the mask: one per viewport size, kept
            destroy_texture(outline_mask_);
            outline_mask_ = create_render_target(w, h);
            outline_size_[0] = w;
            outline_size_[1] = h;
        }
        const f32 clear[4] = {0, 0, 0, 0};
        set_render_target(outline_mask_, clear);
        for (u32 i = 0; i < count; ++i) {  // each as plain white: only where it covers matters
            MeshDraw d = draws[i];
            d.unlit = true;
            d.wireframe = false;
            d.alpha_cutoff = 0.0f;
            d.base_color_texture = {};
            d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1.0f;
            draw_mesh(d);
        }
        f32 k[48] = {};
        k[0] = 1.0f / static_cast<f32>(w);
        k[1] = 1.0f / static_cast<f32>(h);
        for (int c = 0; c < 4; ++c) k[8 + c] = color[c];
        post_pass(kOutline, {}, viewport, outline_mask_, {}, {}, k);
        set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    // Post-processing targets: the bloom chain, the FXAA input and the effects', one set per scene size (a few kept).
    struct PostChain {
        u32 width = 0, height = 0;
        TextureHandle down[5], up[4], ldr;
        TextureHandle ao, lit, dof, motion, taa[2], ssr;
        TextureHandle exposure[2], exposed;  // eye adaptation: this / last frame's exposure (1 x 1), the scene with it
        u32 exposure_index = 0;
        bool exposure_valid = false;
        u32 taa_index = 0;
        bool history_valid = false;  // shortcut: one TAA history per size; two same-size views would share it
    };
    PostChain& post_chain(u32 width, u32 height) {
        for (PostChain& c : post_chains_) {
            if (c.width == width && c.height == height) return c;
        }
        PostChain& c = post_chains_[post_chain_next_++ % 4];
        for (TextureHandle& t : c.down) destroy_texture(t);
        for (TextureHandle& t : c.up) destroy_texture(t);
        for (TextureHandle& t : c.taa) destroy_texture(t);
        for (TextureHandle& t : c.exposure) destroy_texture(t);
        for (TextureHandle t : {c.ao, c.lit, c.dof, c.motion, c.ssr, c.ldr, c.exposed}) destroy_texture(t);
        c = PostChain{};
        c.width = width;
        c.height = height;
        for (u32 i = 0; i < 5; ++i) {
            const u32 w = (width >> (i + 1)) ? (width >> (i + 1)) : 1u, h = (height >> (i + 1)) ? (height >> (i + 1)) : 1u;
            c.down[i] = create_render_target(w, h);
            if (i < 4) c.up[i] = create_render_target(w, h);
        }
        c.ldr = create_render_target(width, height);
        c.ao = create_render_target(width, height);  // shortcut: full-size effect targets even when unused; make them lazily if memory gets tight
        c.lit = create_render_target(width, height);
        c.dof = create_render_target(width, height);
        c.motion = create_render_target(width, height);
        c.ssr = create_render_target(width, height);
        c.taa[0] = create_render_target(width, height);
        c.taa[1] = create_render_target(width, height);
        c.exposure[0] = create_render_target(1, 1);
        c.exposure[1] = create_render_target(1, 1);
        c.exposed = create_render_target(width, height);
        return c;
    }

    // The view post.hlsl reads for one input: a render target's colour (or, as t2 of a target, its
    // depth once readable), an ordinary texture (the Color Grading LUT), else white.
    VkImageView post_input(TextureHandle handle, bool depth) {
        const Texture* t = lookup_texture(handle);
        if (t && depth && t->depth_image) return t->depth_readable ? t->depth_view : lookup_texture(white_)->view;
        if (t && !t->shadow_size) return t->view;
        return lookup_texture(white_)->view;
    }

    // One full-screen pass into `target` (invalid: the window, in `window_rect`).
    void post_pass(PostPass pass, TextureHandle target, const f32* window_rect, TextureHandle in0, TextureHandle in1, TextureHandle in2, const f32 constants[48]) {
        const f32 black[4] = {0, 0, 0, 1};
        if (lookup_texture(target)) {
            set_render_target(target, black);
        } else {
            set_render_target({}, nullptr);
            set_viewport(window_rect[0], window_rect[1], window_rect[2], window_rect[3]);
        }
        Frame& f = frames_[frame_];
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = f.post_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &post_set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!f.post_pool || vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) {
            if (!warned_post_) LSD_LOG_WARN("vulkan", "Too many post-process passes this frame: some are skipped.");
            warned_post_ = true;
            return;
        }
        const VkDescriptorImageInfo images[3] = {{VK_NULL_HANDLE, post_input(in0, false), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, post_input(in1, false), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {VK_NULL_HANDLE, post_input(in2, true), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.descriptorCount = 3;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = images;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipelines_[pass]);
        vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPostPushBytes, constants);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // post.hlsl's resources: t0-t2 (bindings 0-2) and the clamped linear sampler s2 (binding 18,
    // immutable); a descriptor pool per frame for the passes' sets; one pipeline per pass.
    bool create_post_pipelines() {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        if (!check(vkCreateSampler(device_, &sampler, nullptr, &post_sampler_), "vkCreateSampler (post)")) return false;
        const VkDescriptorSetLayoutBinding bindings[4] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                         {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                         {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                         {18, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &post_sampler_}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 4;
        layout.pBindings = bindings;
        if (!check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &post_set_layout_), "vkCreateDescriptorSetLayout (post)")) return false;
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPostPushBytes};
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = 1;
        pipeline_layout.pSetLayouts = &post_set_layout_;
        pipeline_layout.pushConstantRangeCount = 1;
        pipeline_layout.pPushConstantRanges = &push;
        if (!check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &post_layout_), "vkCreatePipelineLayout (post)")) return false;
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kPostSetsPerFrame * 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, kPostSetsPerFrame}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = kPostSetsPerFrame;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        for (Frame& f : frames_) {
            if (!check(vkCreateDescriptorPool(device_, &pool, nullptr, &f.post_pool), "vkCreateDescriptorPool (post)")) return false;
        }
#if LSD_VULKAN_HAVE_SPIRV
        struct Entry {
            const u32* code;
            usize size;
        };
        const Entry entries[kPostPasses] = {
            {kPostPrefilterSpirv, sizeof kPostPrefilterSpirv}, {kPostDownSpirv, sizeof kPostDownSpirv}, {kPostUpSpirv, sizeof kPostUpSpirv},
            {kPostCompositeSpirv, sizeof kPostCompositeSpirv}, {kPostFxaaSpirv, sizeof kPostFxaaSpirv}, {kPostSsrSpirv, sizeof kPostSsrSpirv},
            {kPostAoSpirv, sizeof kPostAoSpirv}, {kPostAoApplySpirv, sizeof kPostAoApplySpirv}, {kPostTaaSpirv, sizeof kPostTaaSpirv},
            {kPostDofSpirv, sizeof kPostDofSpirv}, {kPostMotionSpirv, sizeof kPostMotionSpirv}, {kPostMeterSpirv, sizeof kPostMeterSpirv},
            {kPostExposeSpirv, sizeof kPostExposeSpirv}, {kPostOutlineSpirv, sizeof kPostOutlineSpirv},
        };
        for (u32 i = 0; i < kPostPasses; ++i) {  // the outline is painted over the image; the rest replace it
            if (!make_pipeline(kPostFullSpirv, sizeof kPostFullSpirv, entries[i].code, entries[i].size, nullptr, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kPostPushBytes,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, post_layout_, post_pipelines_[i], false, false, false,
                               i == kOutline ? Blend::Alpha : Blend::Opaque)) return false;
        }
#endif
        return true;
    }
    bool read_render_target(TextureHandle handle, u8* rgba) override {
        Texture* t = lookup_texture(handle);
        if (!t || !t->depth_image || !rgba) return false;
        vkDeviceWaitIdle(device_);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(t->width) * t->height * 4;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool ok = check(vkCreateBuffer(device_, &info, nullptr, &readback), "vkCreateBuffer (readback)");
        if (ok) {
            VkMemoryRequirements need;
            vkGetBufferMemoryRequirements(device_, readback, &need);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = need.size;
            alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ok = alloc.memoryTypeIndex != UINT32_MAX && check(vkAllocateMemory(device_, &alloc, nullptr, &memory), "vkAllocateMemory (readback)") &&
                 check(vkBindBufferMemory(device_, readback, memory, 0), "vkBindBufferMemory (readback)");
        }
        const VkCommandBuffer cmd = ok ? begin_upload() : VK_NULL_HANDLE;
        ok = ok && cmd != VK_NULL_HANDLE;
        if (ok) {
            transition(cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {t->width, t->height, 1};
            vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
            transition(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            ok = end_upload(cmd);
        }
        void* mapped = nullptr;
        if (ok && check(vkMapMemory(device_, memory, 0, bytes, 0, &mapped), "vkMapMemory (readback)")) {
            const u8* src = static_cast<const u8*>(mapped);
            const bool bgra = format_.format == VK_FORMAT_B8G8R8A8_UNORM || format_.format == VK_FORMAT_B8G8R8A8_SRGB;
            for (VkDeviceSize i = 0; i < bytes; i += 4) {  // rows top to bottom, as the target holds them; RGBA
                rgba[i] = src[i + (bgra ? 2 : 0)];
                rgba[i + 1] = src[i + 1];
                rgba[i + 2] = src[i + (bgra ? 0 : 2)];
                rgba[i + 3] = src[i + 3];
            }
            vkUnmapMemory(device_, memory);
        } else {
            ok = false;
        }
        if (readback) vkDestroyBuffer(device_, readback, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
        return ok;
    }
    void draw_particles(const ParticleVertex* vertices, u32 vertex_count, const f32 view_proj[16], TextureHandle texture, bool additive) override {
        const VkPipeline pipeline = particle_pipelines_[additive ? 1 : 0];
        if (skip_frame_ || shadow_pass_ || !pipeline || !vertex_count) return;
        Frame& f = frames_[frame_];
        VkDeviceSize at = 0;
        if (!stream_write(f, vertices, static_cast<VkDeviceSize>(vertex_count) * sizeof(ParticleVertex), at)) return;
        f32 push[kParticlePushBytes / sizeof(f32)] = {};
        std::memcpy(push, view_proj, sizeof(f32) * 16);
        push[16] = scene_.output[0];  // into a scene target: range-compressed
        const Texture* t = lookup_texture(texture);
        if (!t || t->shadow_size) t = lookup_texture(white_);
        if (!t) return;
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, particle_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kParticlePushBytes, push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_layout_, 0, 1, &t->set, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.stream, &at);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    }
    MaterialShaderHandle create_material_shader(const char*) override { return {}; }
    void destroy_material_shader(MaterialShaderHandle) override {}

    void set_viewport(f32 x, f32 y, f32 width, f32 height) override {
        if (skip_frame_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        viewport_[0] = x;
        viewport_[1] = y;
        viewport_[2] = width;
        viewport_[3] = height;
        const VkViewport viewport{x, y + height, width, -height, 0.0f, 1.0f};  // negative height flips Y: clip space as D3D12 and OpenGL (+Y up)
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
        end_pass();
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
        u32 shadow_size = 0;                      // a shadow map (create_shadow_map): its size; 0 = a colour texture
        VkImage depth_image = VK_NULL_HANDLE;     // a render target's depth (create_render_target)
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        u32 width = 0, height = 0;                // a render target's size
        bool depth_readable = false;              // its depth is in the shader-read layout (the post pass reads it)
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
        t.shadow_size = 0;
        if (t.depth_view) vkDestroyImageView(device_, t.depth_view, nullptr);
        if (t.depth_image) vkDestroyImage(device_, t.depth_image, nullptr);
        if (t.depth_memory) vkFreeMemory(device_, t.depth_memory, nullptr);
        t.depth_view = VK_NULL_HANDLE;
        t.depth_image = VK_NULL_HANDLE;
        t.depth_memory = VK_NULL_HANDLE;
        t.width = t.height = 0;
        t.depth_readable = false;
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
        VkSamplerCreateInfo compare{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};  // as D3D12's s1: 2x2 filtered depth compare
        compare.magFilter = VK_FILTER_LINEAR;
        compare.minFilter = VK_FILTER_LINEAR;
        compare.addressModeU = compare.addressModeV = compare.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        compare.compareEnable = VK_TRUE;
        compare.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (!check(vkCreateSampler(device_, &compare, nullptr, &shadow_sampler_), "vkCreateSampler (shadow)")) return false;
        const VkDescriptorSetLayoutBinding shadow_bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                                 {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo shadow_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        shadow_layout.bindingCount = 2;
        shadow_layout.pBindings = shadow_bindings;
        if (!check(vkCreateDescriptorSetLayout(device_, &shadow_layout, nullptr, &shadow_set_layout_), "vkCreateDescriptorSetLayout (shadows)")) return false;
        no_shadow_ = create_shadow_map(1);  // bound for a view without sun or local shadows (all lit)
        return white_.valid() && no_shadow_.valid();
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
        VkBuffer ring = VK_NULL_HANDLE;  // joints and instance matrices written this frame
        VkDeviceMemory ring_memory = VK_NULL_HANDLE;
        u8* ring_mapped = nullptr;
        VkDeviceSize ring_used = 0;
        VkDescriptorSet rows_set = VK_NULL_HANDLE;  // the ring as a dynamic storage buffer
        VkBuffer stream = VK_NULL_HANDLE;  // UI and particle vertices / indices written this frame
        VkDeviceMemory stream_memory = VK_NULL_HANDLE;
        u8* stream_mapped = nullptr;
        VkDeviceSize stream_used = 0;
        VkDescriptorPool post_pool = VK_NULL_HANDLE;  // the post passes' sets, reset each frame
    };

    // Copies `bytes` into the frame's vertex stream; `offset` gets where.
    bool stream_write(Frame& f, const void* data, VkDeviceSize bytes, VkDeviceSize& offset) {
        const VkDeviceSize at = (f.stream_used + 15) & ~VkDeviceSize{15};
        if (!f.stream_mapped || at + bytes > kStreamBytes) {
            if (!warned_stream_) LSD_LOG_WARN("vulkan", "Too many particles / UI vertices this frame: some are skipped.");
            warned_stream_ = true;
            return false;
        }
        std::memcpy(f.stream_mapped + at, data, bytes);
        f.stream_used = at + bytes;
        offset = at;
        return true;
    }

    // Copies `bytes` into the frame's ring; `offset` gets where (for the dynamic descriptor).
    bool ring_write(Frame& f, const void* data, VkDeviceSize bytes, u32& offset) {
        const VkDeviceSize align = storage_alignment_ ? storage_alignment_ : 256;
        const VkDeviceSize at = (f.ring_used + align - 1) / align * align;
        if (!f.ring_mapped || bytes > kRowsWindow || at + kRowsWindow > kRingBytes) {
            if (!warned_ring_) LSD_LOG_WARN("vulkan", "This frame's joint / instance buffer is full: some skinned or instanced meshes are skipped.");
            warned_ring_ = true;
            return false;
        }
        std::memcpy(f.ring_mapped + at, data, bytes);
        f.ring_used = at + bytes;
        offset = static_cast<u32>(at);
        return true;
    }

    // Each frame's ring buffer (persistently mapped) and its descriptor set (a dynamic storage buffer: set 1 rows, set 2 the Scene block).
    bool create_rings() {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // rows and the Scene block
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &rows_set_layout_), "vkCreateDescriptorSetLayout (rows)")) return false;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, kFramesInFlight};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = kFramesInFlight;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (!check(vkCreateDescriptorPool(device_, &pool, nullptr, &rows_pool_), "vkCreateDescriptorPool (rows)")) return false;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_.handle, &props);
        storage_alignment_ = props.limits.minStorageBufferOffsetAlignment;
        for (Frame& f : frames_) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = kRingBytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (!check(vkCreateBuffer(device_, &info, nullptr, &f.ring), "vkCreateBuffer (ring)")) return false;
            VkMemoryRequirements need;
            vkGetBufferMemoryRequirements(device_, f.ring, &need);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = need.size;
            alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void* mapped = nullptr;
            if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &f.ring_memory), "vkAllocateMemory (ring)") ||
                !check(vkBindBufferMemory(device_, f.ring, f.ring_memory, 0), "vkBindBufferMemory (ring)") ||
                !check(vkMapMemory(device_, f.ring_memory, 0, kRingBytes, 0, &mapped), "vkMapMemory (ring)")) return false;
            f.ring_mapped = static_cast<u8*>(mapped);
            VkBufferCreateInfo stream{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stream.size = kStreamBytes;
            stream.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            if (!check(vkCreateBuffer(device_, &stream, nullptr, &f.stream), "vkCreateBuffer (stream)")) return false;
            VkMemoryRequirements sneed;
            vkGetBufferMemoryRequirements(device_, f.stream, &sneed);
            VkMemoryAllocateInfo salloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            salloc.allocationSize = sneed.size;
            salloc.memoryTypeIndex = memory_type(sneed.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void* smapped = nullptr;
            if (salloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &salloc, nullptr, &f.stream_memory), "vkAllocateMemory (stream)") ||
                !check(vkBindBufferMemory(device_, f.stream, f.stream_memory, 0), "vkBindBufferMemory (stream)") ||
                !check(vkMapMemory(device_, f.stream_memory, 0, kStreamBytes, 0, &smapped), "vkMapMemory (stream)")) return false;
            f.stream_mapped = static_cast<u8*>(smapped);
            VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            set_info.descriptorPool = rows_pool_;
            set_info.descriptorSetCount = 1;
            set_info.pSetLayouts = &rows_set_layout_;
            if (!check(vkAllocateDescriptorSets(device_, &set_info, &f.rows_set), "vkAllocateDescriptorSets (rows)")) return false;
            VkDescriptorBufferInfo described{f.ring, 0, kRowsWindow};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = f.rows_set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            write.pBufferInfo = &described;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
        return true;
    }

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
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(gpu_.handle, &supported);
        depth_clamp_ = supported.depthClamp == VK_TRUE;  // shadow casters behind the light's near plane still cast (D3D12's DepthClipEnable off)
        features.features.depthClamp = supported.depthClamp;

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

    // A pipeline for the scene pass (colour + depth) or, `shadow`, a depth-only shadow pass (no pixel
    // shader when ps is null, depth bias, depth clamp): vertex layout, topology, depth test, push constants.
    bool make_pipeline(const u32* vs, usize vs_size, const u32* ps, usize ps_size, const VkVertexInputAttributeDescription* attributes, u32 attribute_count,
                       u32 stride, VkPrimitiveTopology topology, u32 push_bytes, VkShaderStageFlags push_stages, VkPipelineLayout& layout_out, VkPipeline& out,
                       bool textured = false, bool depth_test = true, bool shadow = false, Blend blend_mode = Blend::Opaque) {
        if (!layout_out) {  // pipelines sharing a layout (every mesh and shadow pipeline) make it once
            VkPushConstantRange push{push_stages, 0, push_bytes};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &push;
            // 0 base colour texture, 1 joint / copy rows, 2 the Scene block (both in the frame's ring), 3 the shadow atlases
            const VkDescriptorSetLayout sets[4] = {texture_set_layout_, rows_set_layout_, rows_set_layout_, shadow_set_layout_};
            if (textured) {
                layout.setLayoutCount = 4;
                layout.pSetLayouts = sets;
            }
            if (!check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_out), "vkCreatePipelineLayout (scene)")) return false;
        }
        VkShaderModule modules[2] = {};
        const u32* code[2] = {vs, ps};
        const usize sizes[2] = {vs_size, ps_size};
        const u32 stage_count = ps ? 2 : 1;
        for (u32 i = 0; i < stage_count; ++i) {
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
        if (shadow) {
            raster.depthClampEnable = depth_clamp_ ? VK_TRUE : VK_FALSE;
            raster.depthBiasEnable = VK_TRUE;
            raster.depthBiasSlopeFactor = 2.0f;  // as D3D12's SlopeScaledDepthBias: against acne on slopes
        }
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depth_test && blend_mode == Blend::Opaque ? VK_TRUE : VK_FALSE;  // blended sprites never hide each other
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        if (blend_mode != Blend::Opaque) {  // UI: premultiplied; particles: alpha or additive (as D3D12)
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = blend_mode == Blend::Premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = blend_mode == Blend::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        blend.attachmentCount = shadow ? 0 : 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = shadow ? 0 : 1;
        rendering.pColorAttachmentFormats = &format_.format;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        create.pNext = &rendering;
        create.stageCount = stage_count;
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
        if (modules[1]) vkDestroyShaderModule(device_, modules[1], nullptr);
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
        if (!create_texture_support() || !create_rings() || !create_post_pipelines()) return false;
        const VkVertexInputAttributeDescription skinned_attributes[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SkinnedVertex, uv)},
            {3, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SkinnedVertex, joints)},
            {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SkinnedVertex, weights)},
        };
        const VkVertexInputAttributeDescription mesh_attributes[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)},
        };
        // Shadow casters read only the position and UV (and the skinning attributes).
        const VkVertexInputAttributeDescription shadow_attributes[2] = {mesh_attributes[0], mesh_attributes[2]};
        const VkVertexInputAttributeDescription shadow_skinned_attributes[4] = {skinned_attributes[0], skinned_attributes[2], skinned_attributes[3], skinned_attributes[4]};
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkPrimitiveTopology triangles = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkVertexInputAttributeDescription ui_attributes[3] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, position)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UiVertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)},
        };
        const VkVertexInputAttributeDescription particle_attributes[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleVertex, color)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleVertex, uv)},
        };
        const VkVertexInputAttributeDescription line_attributes[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineVertex, color)},
        };
        return make_pipeline(kMeshVsSpirv, sizeof kMeshVsSpirv, kMeshPsSpirv, sizeof kMeshPsSpirv, mesh_attributes, 3, sizeof(MeshVertex),
                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kMeshPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, mesh_layout_, mesh_pipeline_, true) &&
               make_pipeline(kSkyVsSpirv, sizeof kSkyVsSpirv, kSkyPsSpirv, sizeof kSkyPsSpirv, nullptr, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                             kSkyPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sky_layout_, sky_pipeline_, false, false) &&
               make_pipeline(kMeshSkinnedVsSpirv, sizeof kMeshSkinnedVsSpirv, kMeshSkinnedPsSpirv, sizeof kMeshSkinnedPsSpirv, skinned_attributes, 5, sizeof(SkinnedVertex),
                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kMeshPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, mesh_layout_, skinned_pipeline_, true) &&
               make_pipeline(kMeshInstancedVsSpirv, sizeof kMeshInstancedVsSpirv, kMeshInstancedPsSpirv, sizeof kMeshInstancedPsSpirv, mesh_attributes, 3, sizeof(MeshVertex),
                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kMeshPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, mesh_layout_, instanced_pipeline_, true) &&
               make_pipeline(kShadowVsSpirv, sizeof kShadowVsSpirv, nullptr, 0, shadow_attributes, 2, sizeof(MeshVertex), triangles, kMeshPushBytes, stages,
                             mesh_layout_, shadow_pipelines_[0], true, true, true) &&
               make_pipeline(kShadowSkinnedVsSpirv, sizeof kShadowSkinnedVsSpirv, nullptr, 0, shadow_skinned_attributes, 4, sizeof(SkinnedVertex), triangles,
                             kMeshPushBytes, stages, mesh_layout_, shadow_pipelines_[1], true, true, true) &&
               make_pipeline(kShadowInstancedVsSpirv, sizeof kShadowInstancedVsSpirv, nullptr, 0, shadow_attributes, 2, sizeof(MeshVertex), triangles,
                             kMeshPushBytes, stages, mesh_layout_, shadow_pipelines_[2], true, true, true) &&
               make_pipeline(kShadowVsSpirv, sizeof kShadowVsSpirv, kShadowPsSpirv, sizeof kShadowPsSpirv, shadow_attributes, 2, sizeof(MeshVertex), triangles,
                             kMeshPushBytes, stages, mesh_layout_, shadow_pipelines_[3], true, true, true) &&
               make_pipeline(kUiVsSpirv, sizeof kUiVsSpirv, kUiPsSpirv, sizeof kUiPsSpirv, ui_attributes, 3, sizeof(UiVertex), triangles, kUiPushBytes, stages,
                             ui_layout_, ui_pipeline_, true, false, false, Blend::Premultiplied) &&
               make_pipeline(kParticleVsSpirv, sizeof kParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, particle_attributes, 3, sizeof(ParticleVertex),
                             triangles, kParticlePushBytes, stages, particle_layout_, particle_pipelines_[0], true, true, false, Blend::Alpha) &&
               make_pipeline(kParticleVsSpirv, sizeof kParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, particle_attributes, 3, sizeof(ParticleVertex),
                             triangles, kParticlePushBytes, stages, particle_layout_, particle_pipelines_[1], true, true, false, Blend::Additive) &&
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
    struct Scene {  // mesh.hlsl's Scene (set 2): LightSetup's bytes up to local_tiles, then its output
        u8 prefix[offsetof(LightSetup, fog)] = {};
        f32 output[4] = {};
    };
    static_assert(offsetof(LightSetup, fog) == offsetof(LightSetup, local_tiles) + sizeof(LightSetup::local_tiles), "mesh.hlsl's Scene ends at local_tiles");
    struct ShadowSet {
        VkImageView sun = VK_NULL_HANDLE, local = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    static constexpr u32 kMaxShadowSets = 16;
    ShadowSet shadow_sets_[kMaxShadowSets];
    u32 shadow_set_count_ = 0;
    VkDescriptorSetLayout shadow_set_layout_ = VK_NULL_HANDLE;
    TextureHandle local_shadow_;
    Scene scene_;
    bool scene_in_ring_ = false;  // written into this frame's ring since the last set_lights
    u32 scene_at_ = 0;
    TextureHandle sun_shadow_, no_shadow_;
    Texture* shadow_pass_ = nullptr;   // the shadow map being drawn (between begin / end_shadow_pass)
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipelines_[4] = {};  // static, skinned, instanced, Masked
    bool depth_clamp_ = false;
    f32 viewport_[4] = {};  // the last set_viewport (draw_ui puts it back)
    VkPipelineLayout ui_layout_ = VK_NULL_HANDLE, particle_layout_ = VK_NULL_HANDLE;
    VkPipeline ui_pipeline_ = VK_NULL_HANDLE, particle_pipelines_[2] = {};  // alpha, additive
    bool warned_stream_ = false;
    bool in_pass_ = false;               // between vkCmdBeginRendering and vkCmdEndRendering
    Texture* target_ = nullptr;          // the render target being drawn into (null: the window or a shadow map)
    Texture* resume_target_ = nullptr;   // the target a shadow pass interrupted
    VkSampler post_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkPipeline post_pipelines_[kPostPasses] = {};
    PostChain post_chains_[4];
    u32 post_chain_next_ = 0;
    TextureHandle outline_mask_;
    u32 outline_size_[2] = {};
    bool warned_post_ = false;
    char capture_path_[260] = "";
    Texture textures_[kMaxTextures];
    TextureHandle white_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool texture_pool_ = VK_NULL_HANDLE;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout sky_layout_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rows_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool rows_pool_ = VK_NULL_HANDLE;
    VkPipeline skinned_pipeline_ = VK_NULL_HANDLE, instanced_pipeline_ = VK_NULL_HANDLE;
    VkDeviceSize storage_alignment_ = 0;
    bool warned_ring_ = false;
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
