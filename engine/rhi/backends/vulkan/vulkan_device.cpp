// Vulkan 1.3 backend: adapter enumeration, VK_EXT_memory_budget, swapchain with dynamic rendering and
// a depth buffer, vertex / index buffers, textures (mipmapped, also shown by ImGui), the sky, lines
// and lit meshes (base colour and its texture, Masked, sun and sky ambient), the test triangle
// (SPIR-V compiled from HLSL by DXC at build time) and ImGui.
// Skinned and instanced meshes read their joint / copy matrices from a per-frame ring buffer; the sun
// and the spot / point lights cast shadows (depth-only passes into shadow atlases, compare sampler);
// game UI and particles stream their vertices through a per-frame buffer. Render targets (colour +
// depth) take thumbnails, previews and the scene for the post pass. Lit meshes, the sky and the post
// pass run D3D12's own shaders (engine/rhi/shaders/mesh.hlsl and post.hlsl) compiled to SPIR-V;
// Material Graph shaders compile at runtime through the Vulkan SDK's DXC when it is installed; GPU
// culling of instanced draws runs shaders/cull.hlsl as compute with indirect draws.
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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
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
#include "post_velocity.spv.h"
#include "post_meter.spv.h"
#include "post_expose.spv.h"
#include "post_outline.spv.h"
#include "cull_reset.spv.h"
#include "cull_main.spv.h"
#include "cull_args.spv.h"
#include "hiz_down.spv.h"
#include "morph_main.spv.h"
#include "gpu_particle_sim.spv.h"
#include "gpu_particle_vs.spv.h"
#endif

namespace lsd::rhi {

namespace {

#include "mesh_hlsl.h"              // kMeshHlsl: engine/rhi/shaders/mesh.hlsl (Material Graph shaders start from it)
#include "material_prelude_hlsl.h"  // kMaterialPreludeHlsl
#include "post_hlsl.h"              // kPostHlsl: post-process materials start from it

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxAdapters = 16;
constexpr u32 kMaxSwapchainImages = 8;
constexpr u32 kMaxBuffers = 4096;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// Render targets (scene, post chain, previews) are half float (as D3D12's): only what draws to the window uses the
// swapchain's format.
constexpr VkFormat kTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr u32 kMaxMaterialShaders = 256;
constexpr u32 kMeshPushBytes = 208;  // shaders/mesh.hlsl's Draw constants (52 floats, as D3D12's root constants)
constexpr u32 kDrawSetsPerFrame = 8192;  // material / view descriptor sets a frame (a few per draw at most)
constexpr VkDeviceSize kRowsBytes = kMaxJoints * 48;  // mesh.hlsl's Joints / Instances block: 3 float4 rows each
constexpr VkDeviceSize kParamsBytes = 256;            // its MaterialParams block (16 float4)
constexpr u32 kCullPushBytes = sizeof(CullConstants);  // cull.hlsl's constants (256: the device must allow that many)
constexpr u32 kMaxHizMips = 14;
constexpr u32 kComputeSetsPerFrame = 1024;  // morph and GPU particle sets a frame
constexpr u32 kCullSetsPerFrame = 256;                // gpu_cull draws a frame
constexpr u32 kMaxTextures = 4096;
constexpr VkDeviceSize kRingBytes = 8u << 20;      // per frame in flight: joints and instance matrices
constexpr VkDeviceSize kRowsWindow = 64u << 10;    // what one draw may read from it (the descriptor's range)
constexpr u32 kInstancesPerDraw = static_cast<u32>(kRowsWindow / 48);  // 3 float4 rows per copy
constexpr u32 kShadowPushBytes = 80;  // shadow.hlsl's Push
constexpr u32 kUiPushBytes = 16;      // ui.hlsl's Push
constexpr u32 kParticlePushBytes = 96;  // particle.hlsl's Push
constexpr VkDeviceSize kStreamBytes = kParticleRingBytes;  // per frame in flight: UI and particle vertices
// make_pipeline's colour blending: UI (premultiplied), particles / outline (alpha, additive) and the
// lit meshes' MeshDraw::blend modes, which keep the scene target's alpha (SSR) as D3D12 does.
// The lit meshes' vertex layouts (mesh.hlsl's VsIn: locations in declaration order).
constexpr VkVertexInputAttributeDescription kMeshAttributes[4] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)},
    {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, lightmap_uv)},
};
constexpr VkVertexInputAttributeDescription kSkinnedAttributes[5] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, position)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkinnedVertex, normal)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SkinnedVertex, uv)},
    {3, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(SkinnedVertex, joints)},
    {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SkinnedVertex, weights)},
};

enum class Blend { Opaque, Premultiplied, Alpha, Additive, MeshTranslucent, MeshAdditive, MeshOverdraw };
constexpr u32 kPostPushBytes = 192;  // post.hlsl's Post constants (48 floats)
enum PostPass : u32 { kPrefilter, kDown, kUp, kComposite, kFxaa, kSsr, kAo, kAoApply, kTaa, kDof, kMotion, kMeter, kExpose, kOutline, kVelocity, kPostPasses };
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
    struct Frame;
    struct MaterialShader;
    struct Buffer;

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
            for (MaterialShader& m : material_shaders_) release_material(m);
            for (VkPipeline pipeline : cull_pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
            if (morph_pipeline_) vkDestroyPipeline(device_, morph_pipeline_, nullptr);
            for (VkPipeline pipeline : {gpu_particle_sim_, gpu_sprite_pipelines_[0], gpu_sprite_pipelines_[1]}) {
                if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
            }
            for (VkPipelineLayout layout : {gpu_particle_sim_layout_, gpu_sprite_layout_}) {
                if (layout) vkDestroyPipelineLayout(device_, layout, nullptr);
            }
            for (VkDescriptorSetLayout layout : {gpu_particle_sim_set_layout_, gpu_sprite_set_layout_}) {
                if (layout) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
            }
            if (morph_layout_) vkDestroyPipelineLayout(device_, morph_layout_, nullptr);
            if (morph_set_layout_) vkDestroyDescriptorSetLayout(device_, morph_set_layout_, nullptr);
            release_hiz();
            if (hiz_pipeline_) vkDestroyPipeline(device_, hiz_pipeline_, nullptr);
            if (hiz_layout_) vkDestroyPipelineLayout(device_, hiz_layout_, nullptr);
            if (hiz_set_layout_) vkDestroyDescriptorSetLayout(device_, hiz_set_layout_, nullptr);
            if (hiz_pool_) vkDestroyDescriptorPool(device_, hiz_pool_, nullptr);
            vkDestroyPipelineLayout(device_, cull_layout_, nullptr);
            if (cull_set_layout_) vkDestroyDescriptorSetLayout(device_, cull_set_layout_, nullptr);
            for (Buffer* b : {&cull_out_, &cull_args_}) {
                if (b->buffer) vkDestroyBuffer(device_, b->buffer, nullptr);
                if (b->memory) vkFreeMemory(device_, b->memory, nullptr);
            }
            for (auto& kind : mesh_pipelines_) {
                for (VkPipeline pipeline : kind) vkDestroyPipeline(device_, pipeline, nullptr);
            }
            vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
            vkDestroyPipelineLayout(device_, shadow_layout_, nullptr);
            vkDestroyPipeline(device_, line_pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, line_layout_, nullptr);
            vkDestroyPipeline(device_, sky_pipeline_, nullptr);
            for (VkPipeline shadow : shadow_pipelines_) vkDestroyPipeline(device_, shadow, nullptr);
            vkDestroyPipeline(device_, ui_pipeline_, nullptr);
            vkDestroyPipeline(device_, ui_target_pipeline_, nullptr);
            for (VkPipeline w : post_window_) vkDestroyPipeline(device_, w, nullptr);
            vkDestroyPipeline(device_, particle_pipelines_[0], nullptr);
            vkDestroyPipeline(device_, particle_pipelines_[1], nullptr);
            vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
            vkDestroyPipelineLayout(device_, particle_layout_, nullptr);
            for (VkPipeline post : post_pipelines_) vkDestroyPipeline(device_, post, nullptr);
            for (const PostShader& post : post_shaders_) {
                if (post.pipeline) vkDestroyPipeline(device_, post.pipeline, nullptr);
                if (post.pipeline_window) vkDestroyPipeline(device_, post.pipeline_window, nullptr);
            }
            vkDestroyPipelineLayout(device_, post_layout_, nullptr);
            for (Frame& f : frames_) {
                if (f.post_pool) vkDestroyDescriptorPool(device_, f.post_pool, nullptr);
            }
            if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
            if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
            if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
            for (Frame& f : frames_) {
                if (f.draw_pool) vkDestroyDescriptorPool(device_, f.draw_pool, nullptr);
            }
            if (buffers_pool_) vkDestroyDescriptorPool(device_, buffers_pool_, nullptr);
            for (VkDescriptorSetLayout layout : {material_set_layout_, view_set_layout_, buffers_set_layout_}) {
                if (layout) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
            }
            for (Frame& f : frames_) {
                if (f.ring) vkDestroyBuffer(device_, f.ring, nullptr);
                if (f.ring_memory) vkFreeMemory(device_, f.ring_memory, nullptr);
                if (f.stream) vkDestroyBuffer(device_, f.stream, nullptr);
                if (f.stream_memory) vkFreeMemory(device_, f.stream_memory, nullptr);
            }
            if (rows_pool_) vkDestroyDescriptorPool(device_, rows_pool_, nullptr);
            if (timestamp_pool_) vkDestroyQueryPool(device_, timestamp_pool_, nullptr);
            if (rows_set_layout_) vkDestroyDescriptorSetLayout(device_, rows_set_layout_, nullptr);
            for (Texture& t : textures_) release_texture(t);
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (texture_pool_) vkDestroyDescriptorPool(device_, texture_pool_, nullptr);
            if (texture_set_layout_) vkDestroyDescriptorSetLayout(device_, texture_set_layout_, nullptr);
            if (particle_depth_layout_) vkDestroyDescriptorSetLayout(device_, particle_depth_layout_, nullptr);
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
        read_timestamps();  // this slot's last frame has finished: its GPU timings
        f.ring_used = 0;  // the GPU is done with this frame's ring and stream
        f.stream_used = 0;
        if (f.post_pool) vkResetDescriptorPool(device_, f.post_pool, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &begin);
        if (timestamp_pool_) vkCmdResetQueryPool(f.cmd, timestamp_pool_, frame_ * kGpuTimestamps, kGpuTimestamps);  // outside any pass

        transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transition(f.cmd, depth_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        lights_in_ring_ = false;
        view_set_ = VK_NULL_HANDLE;
        f.memo_count = 0;
        if (f.draw_pool) vkResetDescriptorPool(device_, f.draw_pool, 0);
        run_morphs(f);
        shadow_pass_ = nullptr;
        target_ = nullptr;
        sun_shadow_ = {};
        local_shadow_ = {};
        begin_main_pass(clear_color);
        set_lights(default_lights());  // as D3D12: every view starts from the default look
    }

    // The window pass (colour + depth). clear_color null: carries on after a shadow pass (load what is there).
    void begin_main_pass(const f32* clear_color) {
        in_target_ = false;
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
        in_target_ = true;
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
        info.usage = usage == BufferUsage::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                  : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // the morph pass reads vertices too
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
        bool ok = make_image(width, height, kTargetFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
    bool supports_bc() const override { return bc_; }
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
        switch (format) {
            case TextureFormat::RGBA8_sRGB: image.format = VK_FORMAT_R8G8B8A8_SRGB; break;
            case TextureFormat::BC1: image.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
            case TextureFormat::BC1_sRGB: image.format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; break;
            case TextureFormat::BC3: image.format = VK_FORMAT_BC3_UNORM_BLOCK; break;
            case TextureFormat::BC3_sRGB: image.format = VK_FORMAT_BC3_SRGB_BLOCK; break;
            case TextureFormat::BC5: image.format = VK_FORMAT_BC5_UNORM_BLOCK; break;
            case TextureFormat::RGBA16F: image.format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            default: image.format = VK_FORMAT_R8G8B8A8_UNORM; break;
        }
        if (is_block_compressed(format) && !bc_) return {};
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
        for (u32 m = 0; m < mip_count; ++m) total += texture_bytes(format, std::max(1u, width >> m), std::max(1u, height >> m));
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
                const VkDeviceSize bytes = texture_bytes(format, w, h);
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
        frames_[frame_].memo_count = 0;  // this frame's material / view sets may name its view
        view_set_ = VK_NULL_HANDLE;
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
    void set_lights(const LightSetup& lights) override {  // mesh.hlsl's Lights block, as D3D12 (the viewport filled in)
        lights_ = lights;
        std::memcpy(lights_.viewport, viewport_, sizeof lights_.viewport);
        lights_in_ring_ = false;
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
    void clear_shadow_viewport() override {
        if (skip_frame_ || !shadow_pass_ || !in_pass_) return;
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil.depth = 1.0f;
        VkClearRect rect{};
        rect.rect = {{static_cast<i32>(viewport_[0]), static_cast<i32>(viewport_[1])}, {static_cast<u32>(viewport_[2]), static_cast<u32>(viewport_[3])}};
        rect.layerCount = 1;
        vkCmdClearAttachments(frames_[frame_].cmd, 1, &clear, 1, &rect);
    }

    void begin_shadow_pass(TextureHandle handle, bool clear) override {
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
        depth.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
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

    // shortcut: no GPU culling in shadow passes here (culls_shadow_instances false: the caller culls on the CPU); run_culling
    // pauses colour passes only, a shadow pass's resume for that
    void draw_shadow_instances(BufferHandle vertices, BufferHandle indices, u32 index_count, const f32 view_proj[16], const f32* rows, u32 count,
                               const f32*, const f32*) override {
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
        view_set_ = VK_NULL_HANDLE;
    }
    void set_decal_atlas(TextureHandle atlas) override {
        decal_atlas_ = atlas;
        view_set_ = VK_NULL_HANDLE;
    }
    void set_reflection_atlas(TextureHandle atlas) override {
        reflection_atlas_ = atlas;
        view_set_ = VK_NULL_HANDLE;
    }
    void set_lightmap_atlas(TextureHandle atlas) override {
        lightmap_atlas_ = atlas;
        view_set_ = VK_NULL_HANDLE;
    }
    // As D3D12's draw_mesh: the same constants (mesh.hlsl's Draw) and pipelines per kind (static,
    // skinned, instanced) and mode (opaque, wireframe, translucent, additive, overdraw).
    // shortcut: no Material Graph shaders on Vulkan yet (the built-in material draws); no GPU culling.
    void draw_mesh(const MeshDraw& draw) override {
        if (skip_frame_ || shadow_pass_) return;
        const bool skinned = draw.joint_rows && draw.joint_count;
        const bool instanced = !skinned && draw.instance_rows && draw.instance_count;
        const u32 kind = skinned ? 1u : instanced ? 2u : 0u;
        const u32 mode = draw.wireframe ? 1u : draw.blend >= 1 && draw.blend <= 3 ? 1u + draw.blend : 0u;
        // A Material Graph (not wireframe, not instanced) draws with its own pipelines; the Overdraw mode stays built-in.
        const MaterialShader* material = draw.wireframe || instanced ? nullptr : lookup_material(draw.material_shader);
        const VkPipeline pipeline = material && draw.blend < 3 ? material->pipelines[kind][draw.blend] : mesh_pipelines_[kind][mode];
        const Buffer* vb = lookup(draw.vertices);
        const Buffer* ib = lookup(draw.indices);
        if (!pipeline || !vb || !ib || draw.index_count == 0) return;
        Frame& f = frames_[frame_];
        // GPU culling first (compute), the draws below read what it keeps.
        const u32 cull_batches = (draw.instance_count + kMaxJoints - 1) / kMaxJoints;
        const bool culled = draw.gpu_cull && instanced && run_culling(f, draw, cull_batches);
        if (!lights_in_ring_) {  // this view's Lights block, once per set_lights
            if (!ring_write(f, &lights_, sizeof lights_, lights_at_)) return;
            lights_in_ring_ = true;
        }
        f32 c[kMeshPushBytes / sizeof(f32)] = {};
        std::memcpy(c, draw.mvp, sizeof draw.mvp);
        std::memcpy(c + 16, draw.normal_matrix, sizeof draw.normal_matrix);
        std::memcpy(c + 28, draw.world, sizeof draw.world);
        std::memcpy(c + 40, draw.color, sizeof draw.color);
        c[19] = draw.unlit || draw.wireframe ? 1.0f : static_cast<f32>(draw.debug_view);
        const Texture* normal = lookup_texture(draw.normal_texture);
        if (normal && normal->shadow_size) normal = nullptr;
        const f32 cutoff = draw.alpha_cutoff < 0.0f ? 0.0f : draw.alpha_cutoff > 1.0f ? 1.0f : draw.alpha_cutoff;
        c[23] = (normal ? 1.0f : 0.0f) + (draw.blend ? 2.0f : 0.0f) + 4.0f * std::round(cutoff * 255.0f) + (draw.pixel_art ? 1024.0f : 0.0f);  // see n1.w in the shader
        c[27] = draw.time;  // the normal matrix's spare w
        c[44] = draw.metallic;
        c[45] = draw.roughness;
        c[46] = draw.emissive[0];
        c[47] = draw.emissive[1];
        c[48] = draw.emissive[2];
        std::memcpy(c + 49, draw.camera_position, sizeof draw.camera_position);
        const VkDescriptorSet sets[2] = {material_set(f, view_of(draw.base_color_texture, white_), normal ? normal->view : view_of({}, flat_normal_),
                                                      view_of(draw.orm_texture, white_), view_of(draw.emissive_texture, white_)),
                                         view_set(f)};
        if (!sets[0] || !sets[1]) return;
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kMeshPushBytes, c);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 2, sets, 0, nullptr);
        u32 offsets[3] = {lights_at_, lights_at_, lights_at_};  // Lights, rows, material parameters (unused: any valid offset)
        if (material) {  // its parameters: 16 float4, unset ones 0
            f32 values[kParamsBytes / sizeof(f32)] = {};
            const u32 count = std::min(draw.material_param_count, static_cast<u32>(kParamsBytes / 16));
            if (draw.material_params) std::memcpy(values, draw.material_params, count * 16);
            if (!ring_write(f, values, sizeof values, offsets[2])) return;
        }
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &zero);
        vkCmdBindIndexBuffer(cmd, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
        if (!skinned && !instanced) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1, &f.buffers_set, 3, offsets);
            vkCmdDrawIndexed(cmd, draw.index_count, 1, 0, 0, 0);
            return;
        }
        if (culled) {  // each LOD (its own index buffer) and batch's visible rows from the cull output, its count from the GPU
            CullConstants counts;
            cull_constants(draw, cull_batches, nullptr, hiz_size_, 0, counts);
            const u32 lods = (counts.info[2] >> 1) & 7u;
            for (u32 l = 0; l < lods; ++l) {
                const Buffer* lib = lookup(l ? draw.lod_indices[l - 1] : draw.indices);
                if (!lib) continue;
                vkCmdBindIndexBuffer(cmd, lib->buffer, 0, VK_INDEX_TYPE_UINT32);
                for (u32 b = 0; b < cull_batches; ++b) {
                    offsets[1] = (l * kMaxCulledInstances + b * kMaxJoints) * 48;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1, &f.culled_set, 3, offsets);
                    vkCmdDrawIndexedIndirect(cmd, cull_args_.buffer, 16 + (static_cast<VkDeviceSize>(l) * cull_batches + b) * 20, 1, 20);
                }
            }
            return;
        }
        // Joints (at most kMaxJoints) or the copies in batches of kMaxJoints, as D3D12.
        const u32 total = skinned ? std::min(draw.joint_count, kMaxJoints) : draw.instance_count;
        const f32* rows = skinned ? draw.joint_rows : draw.instance_rows;
        for (u32 first = 0; first < total;) {
            const u32 count = skinned ? total : std::min(total - first, kMaxJoints);
            if (!ring_write(f, rows + static_cast<usize>(first) * 12, static_cast<VkDeviceSize>(count) * 48, offsets[1])) return;  // full this frame: the rest waits
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1, &f.buffers_set, 3, offsets);
            vkCmdDrawIndexed(cmd, draw.index_count, skinned ? 1 : count, 0, 0, 0);
            first += count;
        }
    }

    bool supports_gpu_culling() const override { return cull_pipelines_[2] != VK_NULL_HANDLE; }
    bool supports_gpu_morphs() const override { return morph_pipeline_ != VK_NULL_HANDLE; }
    BufferHandle create_morph_output(u32 vertex_count) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].buffer) ++slot;
        if (slot == kMaxBuffers || vertex_count == 0) return {};
        Buffer& b = buffers_[slot];
        const VkDeviceSize size = static_cast<VkDeviceSize>(vertex_count) * sizeof(SkinnedVertex);
        if (!make_device_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, b)) return {};
        b.size = size;
        return BufferHandle::make(slot, b.generation);
    }
    void morph_vertices(BufferHandle out, BufferHandle base, BufferHandle morphs, u32 vertex_count, const f32* weights, u32 weight_count) override {
        MorphJob* job = nullptr;  // one job per output: a later change this frame replaces it
        for (MorphJob& j : pending_morphs_) {
            if (j.out.value == out.value) job = &j;
        }
        if (!job) job = &pending_morphs_.emplace_back();
        morph_job(*job, out, base, morphs, vertex_count, weights, weight_count);
    }
    // GPU particles: the compute pass (set 0: parameters, particles) and the sprites (the particle layout's sets plus
    // set 2: parameters and particles for the vertex shader; no vertex input).
    bool create_gpu_particles(VkShaderStageFlags stages, VkPrimitiveTopology triangles) {
#if LSD_VULKAN_HAVE_SPIRV
        auto set_layout = [&](VkShaderStageFlags stage, VkDescriptorSetLayout& out) {
            const VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, nullptr},
                                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stage, nullptr}};
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = 2;
            info.pBindings = bindings;
            return check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &out), "vkCreateDescriptorSetLayout (GPU particles)");
        };
        if (!set_layout(VK_SHADER_STAGE_COMPUTE_BIT, gpu_particle_sim_set_layout_) || !set_layout(VK_SHADER_STAGE_VERTEX_BIT, gpu_sprite_set_layout_)) return false;
        VkPipelineLayoutCreateInfo sim_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sim_layout.setLayoutCount = 1;
        sim_layout.pSetLayouts = &gpu_particle_sim_set_layout_;
        if (!check(vkCreatePipelineLayout(device_, &sim_layout, nullptr, &gpu_particle_sim_layout_), "vkCreatePipelineLayout (GPU particles)")) return false;
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = sizeof kGpuParticleSimSpirv;
        module_info.pCode = kGpuParticleSimSpirv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!check(vkCreateShaderModule(device_, &module_info, nullptr, &module), "vkCreateShaderModule (GPU particles)")) return false;
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = module;
        create.stage.pName = "main";
        create.layout = gpu_particle_sim_layout_;
        const bool ok = check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &gpu_particle_sim_), "vkCreateComputePipelines (GPU particles)");
        vkDestroyShaderModule(device_, module, nullptr);
        if (!ok) return false;
        const VkPushConstantRange push{stages, 0, kParticlePushBytes};
        const VkDescriptorSetLayout sets[3] = {texture_set_layout_, particle_depth_layout_, gpu_sprite_set_layout_};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 3;
        layout.pSetLayouts = sets;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (!check(vkCreatePipelineLayout(device_, &layout, nullptr, &gpu_sprite_layout_), "vkCreatePipelineLayout (GPU sprites)")) return false;
        if (make_pipeline(kGpuParticleVsSpirv, sizeof kGpuParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, nullptr, 0, 0, triangles, kParticlePushBytes, stages,
                          gpu_sprite_layout_, gpu_sprite_pipelines_[0], true, true, false, Blend::Alpha) &&
            make_pipeline(kGpuParticleVsSpirv, sizeof kGpuParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, nullptr, 0, 0, triangles, kParticlePushBytes, stages,
                          gpu_sprite_layout_, gpu_sprite_pipelines_[1], true, true, false, Blend::Additive)) return true;
        gpu_sprite_pipelines_[0] = gpu_sprite_pipelines_[1] = VK_NULL_HANDLE;
        return false;
#else
        (void)stages, (void)triangles;
        return false;
#endif
    }
    // Morph targets (cull.hlsl's morph_main): four storage buffers (base, packed targets, weights in the ring, output).
    bool create_morphs() {
#if LSD_VULKAN_HAVE_SPIRV
        VkDescriptorSetLayoutBinding bindings[4];
        for (u32 k = 0; k < 4; ++k) bindings[k] = {k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 4;
        layout.pBindings = bindings;
        if (!check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &morph_set_layout_), "vkCreateDescriptorSetLayout (morph)")) return false;
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = 1;
        pipeline_layout.pSetLayouts = &morph_set_layout_;
        if (!check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &morph_layout_), "vkCreatePipelineLayout (morph)")) return false;
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = sizeof kMorphMainSpirv;
        module_info.pCode = kMorphMainSpirv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!check(vkCreateShaderModule(device_, &module_info, nullptr, &module), "vkCreateShaderModule (morph)")) return false;
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = module;
        create.stage.pName = "main";
        create.layout = morph_layout_;
        const bool ok = check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &morph_pipeline_), "vkCreateComputePipelines (morph)");
        vkDestroyShaderModule(device_, module, nullptr);
        return ok;
#else
        return false;
#endif
    }
    // The morphs queued since the last frame, before its first pass; the outputs' last reads before, the draws' after.
    void run_morphs(Frame& f) {
        if (pending_morphs_.empty()) return;
        bool barrier = false;
        for (const MorphJob& j : pending_morphs_) {
            const Buffer* out = lookup(j.out);
            const Buffer* base = lookup(j.base);
            const Buffer* morphs = lookup(j.morphs);
            if (!morph_pipeline_ || !out || !base || !morphs) continue;
            const VkDeviceSize bytes = j.word_count * 4;
            const VkDeviceSize align = storage_alignment_ ? storage_alignment_ : 256;
            const VkDeviceSize at = (f.ring_used + align - 1) / align * align;
            if (!f.ring_mapped || at + bytes > kRingBytes) break;
            std::memcpy(f.ring_mapped + at, j.words, bytes);
            f.ring_used = at + bytes;
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = f.draw_pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &morph_set_layout_;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) break;
            const VkDescriptorBufferInfo buffers[4] = {{base->buffer, 0, VK_WHOLE_SIZE}, {morphs->buffer, 0, VK_WHOLE_SIZE}, {f.ring, at, bytes}, {out->buffer, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.descriptorCount = 4;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = buffers;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            if (!barrier) {  // earlier frames' vertex reads of the outputs before these writes
                memory_barrier(f.cmd, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, morph_pipeline_);
                barrier = true;
            }
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, morph_layout_, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(f.cmd, (j.vertex_count + 63) / 64, 1, 1);
        }
        if (barrier) memory_barrier(f.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        pending_morphs_.clear();
    }

    // The compute passes for a gpu_cull draw, between a pause and a resume of the pass (compute cannot
    // run inside one); false (nothing recorded) if it cannot run: the draw draws every copy.
    bool run_culling(Frame& f, const MeshDraw& draw, u32 batches) {
        if (!cull_pipelines_[2] || draw.instance_count > kMaxCulledInstances) return false;
        CullConstants cull;
        cull_constants(draw, batches, hiz_valid_ ? hiz_view_proj_ : nullptr, hiz_size_, hiz_mips_, cull);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(draw.instance_count) * 48;
        const VkDeviceSize align = storage_alignment_ ? storage_alignment_ : 256;
        const VkDeviceSize at = (f.ring_used + align - 1) / align * align;
        if (!f.ring_mapped || at + bytes + kRowsWindow > kRingBytes) return false;  // the rows go in whole (not a window): leave the ring's usual room
        std::memcpy(f.ring_mapped + at, draw.instance_rows, bytes);
        f.ring_used = at + bytes;
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = f.draw_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &cull_set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo buffers[3] = {{f.ring, at, bytes}, {cull_out_.buffer, 0, VK_WHOLE_SIZE}, {cull_args_.buffer, 0, VK_WHOLE_SIZE}};
        // The Hi-Z (kept in GENERAL), else white: read only when the constants say so.
        const VkDescriptorImageInfo hiz{VK_NULL_HANDLE, hiz_valid_ ? hiz_view_ : lookup_texture(white_)->view,
                                        hiz_valid_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = set;
        writes[0].descriptorCount = 3;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = buffers;
        writes[1].dstSet = set;
        writes[1].dstBinding = 3;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &hiz;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        const VkCommandBuffer cmd = f.cmd;
        Texture* resume = target_;
        const f32 viewport[4] = {viewport_[0], viewport_[1], viewport_[2], viewport_[3]};
        end_pass();
        // The last culled draw's reads (arguments, rows) before these writes; then each pass's writes before the next.
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, cull_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, kCullPushBytes, &cull);
        const u32 groups[3] = {1, (draw.instance_count + 63) / 64, (batches * 4 + 63) / 64};
        for (u32 k = 0; k < 3; ++k) {
            if (k) memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipelines_[k]);
            vkCmdDispatch(cmd, groups[k], 1, 1);
        }
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);
        if (resume) begin_target_pass(*resume, nullptr);
        else begin_main_pass(nullptr);
        set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        return true;
    }

    static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // GPU culling's resources: the passes (cull.hlsl), its output rows and indirect arguments (device
    // memory) and each frame's set 2 reading the rows from that output. Optional: without them gpu_cull
    // draws draw every copy.
    bool create_culling() {
#if LSD_VULKAN_HAVE_SPIRV
        VkPhysicalDeviceProperties limits;
        vkGetPhysicalDeviceProperties(gpu_.handle, &limits);
        if (limits.limits.maxPushConstantsSize < kCullPushBytes) {
            LSD_LOG_INFO("vulkan", "GPU culling off: the device allows %u bytes of push constants (it needs %u).", limits.limits.maxPushConstantsSize, kCullPushBytes);
            return true;
        }
        const VkDescriptorSetLayoutBinding bindings[4] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                         {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 4;
        layout.pBindings = bindings;
        if (!check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &cull_set_layout_), "vkCreateDescriptorSetLayout (cull)")) return false;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kCullPushBytes};
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = 1;
        pipeline_layout.pSetLayouts = &cull_set_layout_;
        pipeline_layout.pushConstantRangeCount = 1;
        pipeline_layout.pPushConstantRanges = &push;
        if (!check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &cull_layout_), "vkCreatePipelineLayout (cull)")) return false;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (!make_device_buffer(4 * static_cast<VkDeviceSize>(kMaxCulledInstances) * 48, usage, cull_out_) ||  // each LOD's rows
            !make_device_buffer(16 + 4 * static_cast<VkDeviceSize>(kMaxCulledInstances / kMaxJoints) * 20, usage, cull_args_)) return false;
        for (Frame& f : frames_) {
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = buffers_pool_;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &buffers_set_layout_;
            if (!check(vkAllocateDescriptorSets(device_, &info, &f.culled_set), "vkAllocateDescriptorSets (culled)")) return false;
            const VkDescriptorBufferInfo ranges[3] = {{f.ring, 0, sizeof(LightSetup)}, {cull_out_.buffer, 0, kRowsBytes}, {f.ring, 0, kParamsBytes}};
            VkWriteDescriptorSet writes[3] = {};
            for (u32 i = 0; i < 3; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = f.culled_set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[i].pBufferInfo = &ranges[i];
            }
            vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
        }
        const u32* code[3] = {kCullResetSpirv, kCullMainSpirv, kCullArgsSpirv};
        const usize sizes[3] = {sizeof kCullResetSpirv, sizeof kCullMainSpirv, sizeof kCullArgsSpirv};
        for (u32 k = 0; k < 3; ++k) {
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = sizes[k];
            module_info.pCode = code[k];
            VkShaderModule module = VK_NULL_HANDLE;
            if (!check(vkCreateShaderModule(device_, &module_info, nullptr, &module), "vkCreateShaderModule (cull)")) return false;
            VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            create.stage.module = module;
            create.stage.pName = "main";
            create.layout = cull_layout_;
            const bool ok = check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &cull_pipelines_[k]), "vkCreateComputePipelines (cull)");
            vkDestroyShaderModule(device_, module, nullptr);
            if (!ok) return false;
        }
        // The Hi-Z builder (cull.hlsl's hiz_down): its source (sampled) and destination (storage) per dispatch, 16 bytes of sizes.
        const VkDescriptorSetLayoutBinding hiz_bindings[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo hiz_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        hiz_layout.bindingCount = 2;
        hiz_layout.pBindings = hiz_bindings;
        if (!check(vkCreateDescriptorSetLayout(device_, &hiz_layout, nullptr, &hiz_set_layout_), "vkCreateDescriptorSetLayout (Hi-Z)")) return true;
        const VkPushConstantRange hiz_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo hiz_pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        hiz_pipeline_layout.setLayoutCount = 1;
        hiz_pipeline_layout.pSetLayouts = &hiz_set_layout_;
        hiz_pipeline_layout.pushConstantRangeCount = 1;
        hiz_pipeline_layout.pPushConstantRanges = &hiz_push;
        if (!check(vkCreatePipelineLayout(device_, &hiz_pipeline_layout, nullptr, &hiz_layout_), "vkCreatePipelineLayout (Hi-Z)")) return true;
        const VkDescriptorPoolSize hiz_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight * kMaxHizMips}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight * kMaxHizMips}};
        VkDescriptorPoolCreateInfo hiz_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        hiz_pool.maxSets = kFramesInFlight * kMaxHizMips;
        hiz_pool.poolSizeCount = 2;
        hiz_pool.pPoolSizes = hiz_sizes;
        if (!check(vkCreateDescriptorPool(device_, &hiz_pool, nullptr, &hiz_pool_), "vkCreateDescriptorPool (Hi-Z)")) return true;
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = sizeof kHizDownSpirv;
        module_info.pCode = kHizDownSpirv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!check(vkCreateShaderModule(device_, &module_info, nullptr, &module), "vkCreateShaderModule (Hi-Z)")) return true;
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = module;
        create.stage.pName = "main";
        create.layout = hiz_layout_;
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &hiz_pipeline_), "vkCreateComputePipelines (Hi-Z)");
        vkDestroyShaderModule(device_, module, nullptr);
#endif
        return true;
    }

    void release_hiz() {
        if (hiz_image_) vkDeviceWaitIdle(device_);
        for (VkImageView& v : hiz_mip_views_) {
            if (v) vkDestroyImageView(device_, v, nullptr);
            v = VK_NULL_HANDLE;
        }
        if (hiz_view_) vkDestroyImageView(device_, hiz_view_, nullptr);
        if (hiz_image_) vkDestroyImage(device_, hiz_image_, nullptr);
        if (hiz_memory_) vkFreeMemory(device_, hiz_memory_, nullptr);
        hiz_view_ = VK_NULL_HANDLE;
        hiz_image_ = VK_NULL_HANDLE;
        hiz_memory_ = VK_NULL_HANDLE;
        hiz_valid_ = false;
    }

    // The Hi-Z (PostDraw::build_hiz) from a scene target's readable depth: half its size, halving to 1 x 1, each texel the
    // farthest of 2 x 2. The image stays in GENERAL (written by these dispatches, read by the cull pass). Outside a pass.
    void build_hiz(Texture& scene, const f32 view_proj[16]) {
        if (!hiz_pipeline_ || !scene.depth_readable) return;
        const u32 w = scene.width > 1 ? (scene.width + 1) / 2 : 1, h = scene.height > 1 ? (scene.height + 1) / 2 : 1;
        u32 mips = 1;
        while ((std::max(w, h) >> mips) > 0 && mips < kMaxHizMips) ++mips;
        if (!hiz_image_ || hiz_size_[0] != w || hiz_size_[1] != h) {
            release_hiz();
            vkResetDescriptorPool(device_, hiz_pool_, 0);
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R32_SFLOAT;
            info.extent = {w, h, 1};
            info.mipLevels = mips;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (!check(vkCreateImage(device_, &info, nullptr, &hiz_image_), "vkCreateImage (Hi-Z)")) return;
            VkMemoryRequirements need;
            vkGetImageMemoryRequirements(device_, hiz_image_, &need);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = need.size;
            alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (alloc.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(device_, &alloc, nullptr, &hiz_memory_), "vkAllocateMemory (Hi-Z)") ||
                !check(vkBindImageMemory(device_, hiz_image_, hiz_memory_, 0), "vkBindImageMemory (Hi-Z)")) {
                release_hiz();
                return;
            }
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = hiz_image_;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_R32_SFLOAT;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
            check(vkCreateImageView(device_, &view, nullptr, &hiz_view_), "vkCreateImageView (Hi-Z)");
            for (u32 m = 0; m < mips; ++m) {
                view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
                check(vkCreateImageView(device_, &view, nullptr, &hiz_mip_views_[m]), "vkCreateImageView (Hi-Z mip)");
            }
            for (u32 fi = 0; fi < kFramesInFlight; ++fi) {
                for (u32 m = 0; m < mips; ++m) {
                    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    set_info.descriptorPool = hiz_pool_;
                    set_info.descriptorSetCount = 1;
                    set_info.pSetLayouts = &hiz_set_layout_;
                    if (vkAllocateDescriptorSets(device_, &set_info, &hiz_sets_[fi][m]) != VK_SUCCESS) {
                        release_hiz();
                        return;
                    }
                }
            }
            const VkCommandBuffer up = begin_upload();
            if (!up) {
                release_hiz();
                return;
            }
            transition(up, hiz_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mips);
            end_upload(up);
            hiz_size_[0] = w;
            hiz_size_[1] = h;
            hiz_mips_ = mips;
        }
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        // The last frame's cull reads before these writes; each mip's writes before the next reads them.
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_);
        u32 sw = scene.width, sh = scene.height;
        for (u32 m = 0; m < hiz_mips_; ++m) {
            const u32 dw = std::max(hiz_size_[0] >> m, 1u), dh = std::max(hiz_size_[1] >> m, 1u);
            const VkDescriptorSet set = hiz_sets_[frame_][m];
            const VkDescriptorImageInfo src{VK_NULL_HANDLE, m ? hiz_mip_views_[m - 1] : scene.depth_view, m ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo dst{VK_NULL_HANDLE, hiz_mip_views_[m], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            writes[0].dstSet = writes[1].dstSet = set;
            writes[0].descriptorCount = writes[1].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[0].pImageInfo = &src;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &dst;
            vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
            const u32 size[4] = {dw, dh, sw, sh};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_layout_, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, size);
            vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
            memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            sw = dw;
            sh = dh;
        }
        std::memcpy(hiz_view_proj_, view_proj, sizeof hiz_view_proj_);
        hiz_valid_ = true;
    }

    bool make_device_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        if (!check(vkCreateBuffer(device_, &info, nullptr, &out.buffer), "vkCreateBuffer (cull)")) return false;
        VkMemoryRequirements need;
        vkGetBufferMemoryRequirements(device_, out.buffer, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        out.size = size;
        return alloc.memoryTypeIndex != UINT32_MAX && check(vkAllocateMemory(device_, &alloc, nullptr, &out.memory), "vkAllocateMemory (cull)") &&
               check(vkBindBufferMemory(device_, out.buffer, out.memory, 0), "vkBindBufferMemory (cull)");
    }

    // A texture's view for the lit shader (not a shadow map), else the fallback's (white, flat normal).
    VkImageView view_of(TextureHandle handle, TextureHandle fallback) {
        const Texture* t = lookup_texture(handle);
        if (!t || t->shadow_size) t = lookup_texture(fallback);
        return t ? t->view : VK_NULL_HANDLE;
    }

    // Set 0: a material's base colour, normal, ORM and emissive textures; the frame's last few kept (draws repeat them).
    VkDescriptorSet material_set(Frame& f, VkImageView base, VkImageView normal, VkImageView orm, VkImageView emissive) {
        if (!base || !normal || !orm || !emissive) return VK_NULL_HANDLE;
        for (u32 i = 0; i < std::min(f.memo_count, kMaterialMemo); ++i) {
            const MaterialMemo& m = f.memo[i];
            if (m.views[0] == base && m.views[1] == normal && m.views[2] == orm && m.views[3] == emissive) return m.set;
        }
        const VkImageView views[4] = {base, normal, orm, emissive};
        const VkDescriptorSet set = allocate_set(f, material_set_layout_, views, 3);
        if (set) {  // emissive at binding 4 (3 is the sampler)
            const VkDescriptorImageInfo image{VK_NULL_HANDLE, emissive, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = 4;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            MaterialMemo& m = f.memo[f.memo_count++ % kMaterialMemo];
            std::memcpy(m.views, views, sizeof views);
            m.set = set;
        }
        return set;
    }

    // Set 1: the view's shadow maps (the far 1x1 one for a missing one), decal, reflection and lightmap atlases (white when unset).
    VkDescriptorSet view_set(Frame& f) {
        if (view_set_) return view_set_;
        auto shadow_view = [&](TextureHandle h) {
            const Texture* t = lookup_texture(h);
            if (!t || !t->shadow_size) t = lookup_texture(no_shadow_);
            return t ? t->view : VK_NULL_HANDLE;
        };
        const VkImageView views[5] = {shadow_view(sun_shadow_), shadow_view(local_shadow_), view_of(decal_atlas_, white_), view_of(reflection_atlas_, white_),
                                      view_of(lightmap_atlas_, white_)};
        for (VkImageView v : views) {
            if (!v) return VK_NULL_HANDLE;
        }
        view_set_ = allocate_set(f, view_set_layout_, views, 5);
        return view_set_;
    }

    // A set from the frame's pool with `count` sampled images at bindings 0.. (its samplers are immutable).
    VkDescriptorSet allocate_set(Frame& f, VkDescriptorSetLayout layout, const VkImageView* views, u32 count) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = f.draw_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!f.draw_pool || vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) {
            if (!warned_sets_) LSD_LOG_WARN("vulkan", "Too many material / view changes this frame: some meshes are skipped.");
            warned_sets_ = true;
            return VK_NULL_HANDLE;
        }
        VkDescriptorImageInfo images[5] = {};
        for (u32 i = 0; i < count; ++i) images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.descriptorCount = count;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = images;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return set;
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
        vkCmdPushConstants(cmd, shadow_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kShadowPushBytes, push);
        if (masked) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_layout_, 0, 1, &masked, 0, nullptr);
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
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_layout_, 1, 1, &f.rows_set, 1, &at);
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
    void draw_sky(const SkyDraw& sky) override {  // mesh.hlsl's sky through the Draw constants, as D3D12
        if (skip_frame_ || shadow_pass_ || !sky_pipeline_) return;
        Frame& f = frames_[frame_];
        if (!lights_in_ring_) {
            if (!ring_write(f, &lights_, sizeof lights_, lights_at_)) return;
            lights_in_ring_ = true;
        }
        f32 c[kMeshPushBytes / sizeof(f32)] = {};
        std::memcpy(c, sky.inv_view_proj, sizeof sky.inv_view_proj);
        std::memcpy(c + 16, sky.zenith, 16);
        std::memcpy(c + 20, sky.horizon, 16);
        std::memcpy(c + 24, sky.ground, 16);
        std::memcpy(c + 28, sky.sun_direction, 16);
        std::memcpy(c + 32, sky.sun_color, 16);
        std::memcpy(c + 49, sky.camera, 12);
        const u32 offsets[3] = {lights_at_, lights_at_, lights_at_};
        const VkDescriptorSet view = view_set(f);  // the reflection atlas: an HDRI's tile
        if (!view) return;
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 1, 1, &view, 0, nullptr);
        vkCmdPushConstants(cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kMeshPushBytes, c);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 2, 1, &f.buffers_set, 3, offsets);
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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, in_target_ ? ui_target_pipeline_ : ui_pipeline_);
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
        k[2] = lookup_texture(p.normals) ? 1.0f : 0.0f;  // SSR / AO: normals from the normals pass (t1)
        end_pass();  // the scene's colour (and depth below) become readable
        if ((ao || dof || taa || ssr || motion || p.build_hiz) && !scene->depth_readable) {
            const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            transition(frames_[frame_].cmd, scene->depth_image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tests,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            scene->depth_readable = true;
        }
        if (p.build_hiz) build_hiz(*scene, p.hiz_view_proj);  // the next frame's occlusion test
        TextureHandle src = p.scene;
        if (ssr) {  // first: it reads the scene's alpha (reflectivity)
            post_pass(kSsr, c.ssr, nullptr, p.scene, p.normals, p.scene, k);
            src = c.ssr;
        }
        if (ao) {
            f32 ka[48];  // the AO pass reads no reprojection: its first row carries the contact shadow ray
            std::memcpy(ka, k, sizeof ka);
            std::memcpy(ka + 32, p.contact_shadow, sizeof p.contact_shadow);
            post_pass(kAo, c.ao, nullptr, {}, p.normals, p.scene, ka);
            post_pass(kAoApply, c.lit, nullptr, src, c.ao, p.scene, k);
            src = c.lit;
        }
        // The velocity pass (objects' own movement): resolved with the camera's where nothing was drawn, for TAA and motion blur.
        const bool vel = (taa || motion) && lookup_texture(p.velocity);
        f32 kv[48];
        std::memcpy(kv, k, sizeof kv);
        kv[3] = 1.0f;  // texel.w: they read it
        if (vel) post_pass(kVelocity, c.velocity, nullptr, {}, p.velocity, p.scene, k);
        if (taa) {
            const TextureHandle out = c.taa[c.taa_index], history = c.taa[c.taa_index ^ 1u];
            post_pass(kTaa, out, nullptr, src, history, vel ? c.velocity : p.scene, vel ? kv : k, VK_NULL_HANDLE, vel);
            c.taa_index ^= 1u;
            c.history_valid = true;
            src = out;
        }
        if (dof) {
            post_pass(kDof, c.dof, nullptr, src, {}, p.scene, k);
            src = c.dof;
        }
        if (motion) {
            post_pass(kMotion, c.motion, nullptr, src, vel ? c.velocity : TextureHandle{}, p.scene, vel ? kv : k);
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
        post_color_vision(p, g);
        const bool fxaa = p.fxaa && !taa;
        const PostShader* custom = lookup_post(p.post_shader);
        if (custom) {  // a post-process material: the scene's texel, its time and parameters (proj's row: unused here)
            size(src, g);
            g[16] = p.post_time;
            for (int n = 0; n < 3; ++n) g[17 + n] = p.post_params[n];
        }
        const bool to_window = !fxaa && !lookup_texture(p.output);
        post_pass(kComposite, fxaa ? c.ldr : p.output, p.viewport, src, bloom ? c.up[0] : TextureHandle{}, p.color_lut, g,
                  custom ? (to_window ? custom->pipeline_window : custom->pipeline) : to_window ? post_window_[0] : VK_NULL_HANDLE);
        if (fxaa) {
            f32 f[48] = {};
            size(c.ldr, f);
            post_pass(kFxaa, p.output, p.viewport, c.ldr, {}, {}, f, lookup_texture(p.output) ? VK_NULL_HANDLE : post_window_[1]);
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
        TextureHandle ao, lit, dof, motion, taa[2], ssr, velocity;
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
        for (TextureHandle t : {c.ao, c.lit, c.dof, c.motion, c.ssr, c.ldr, c.exposed, c.velocity}) destroy_texture(t);
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
        c.velocity = create_render_target(width, height);
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
    // `custom`: a post-process material's composite pipeline instead of the pass's.
    void post_pass(PostPass pass, TextureHandle target, const f32* window_rect, TextureHandle in0, TextureHandle in1, TextureHandle in2, const f32 constants[48],
                   VkPipeline custom = VK_NULL_HANDLE, bool color2 = false) {  // color2: in2's colour, not its depth
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
                                                 {VK_NULL_HANDLE, post_input(in2, !color2), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.descriptorCount = 3;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = images;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, custom ? custom : post_pipelines_[pass]);
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
            {kPostExposeSpirv, sizeof kPostExposeSpirv}, {kPostOutlineSpirv, sizeof kPostOutlineSpirv}, {kPostVelocitySpirv, sizeof kPostVelocitySpirv},
        };
        for (u32 i = 0; i < kPostPasses; ++i) {  // the outline is painted over the image; the rest replace it
            auto make = [&](VkPipeline& out) {
                return make_pipeline(kPostFullSpirv, sizeof kPostFullSpirv, entries[i].code, entries[i].size, nullptr, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kPostPushBytes,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, post_layout_, out, false, false, false,
                                     i == kOutline ? Blend::Alpha : Blend::Opaque);
            };
            // The outline paints over the window; composite and FXAA write into a target or to the window: one of each.
            if (i == kOutline ? !window_pipeline([&] { return make(post_pipelines_[i]); }) : !make(post_pipelines_[i])) return false;
            if ((i == kComposite || i == kFxaa) && !window_pipeline([&] { return make(post_window_[i == kComposite ? 0 : 1]); })) return false;
        }
#endif
        return true;
    }
    bool read_render_target(TextureHandle handle, u8* rgba) override {
        Texture* t = lookup_texture(handle);
        if (!t || !t->depth_image || !rgba) return false;
        vkDeviceWaitIdle(device_);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(t->width) * t->height * 8;  // half floats
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
            const u16* src = static_cast<const u16*>(mapped);
            for (VkDeviceSize i = 0; i < bytes / 2; ++i) {  // rows top to bottom, as the target holds them; RGBA, 0..1 to bytes
                const u16 h = src[i];
                const u32 exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
                const f32 v = (h & 0x8000u) ? 0.0f : exponent == 0 ? std::ldexp(static_cast<f32>(mantissa) / 1024.0f, -14)
                            : exponent == 31 ? 1.0f : std::ldexp(1.0f + static_cast<f32>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
                rgba[i] = static_cast<u8>(std::lround(std::fmin(v, 1.0f) * 255.0f));
            }
            vkUnmapMemory(device_, memory);
        } else {
            ok = false;
        }
        if (readback) vkDestroyBuffer(device_, readback, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
        return ok;
    }
    // Soft particles: the target's depth goes read-only (tested and sampled at once) for a draw, then back.
    void target_depth_read_only(bool read_only) {
        Texture& t = *target_;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        vkCmdEndRendering(cmd);
        const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        const VkAccessFlags2 rw = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (read_only) {
            transition(cmd, t.depth_image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, tests, rw,
                       tests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            transition(cmd, t.depth_image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, tests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       0, tests, rw, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = t.depth_view;
        depth.imageLayout = read_only ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = read_only ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {t.width, t.height};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
        set_viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    }

    void draw_particles(const ParticleVertex* vertices, u32 vertex_count, const f32 view_proj[16], TextureHandle texture, bool additive, const f32* soft) override {
        const VkPipeline pipeline = particle_pipelines_[additive ? 1 : 0];
        if (skip_frame_ || shadow_pass_ || !pipeline || !vertex_count) return;
        Frame& f = frames_[frame_];
        VkDeviceSize at = 0;
        if (!stream_write(f, vertices, static_cast<VkDeviceSize>(vertex_count) * sizeof(ParticleVertex), at)) return;
        draw_sprites(f, pipeline, particle_layout_, vertex_count, view_proj, texture, soft, VK_NULL_HANDLE, &at);
    }

    bool supports_gpu_particles() const override { return gpu_sprite_pipelines_[1] != VK_NULL_HANDLE; }
    BufferHandle create_particle_state(u32 capacity) override {
        u32 slot = 0;
        while (slot < kMaxBuffers && buffers_[slot].buffer) ++slot;
        if (slot == kMaxBuffers || capacity == 0 || capacity > kMaxGpuParticles) return {};
        Buffer& b = buffers_[slot];
        const VkDeviceSize size = static_cast<VkDeviceSize>(capacity) * 48;
        if (!make_device_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, b)) return {};
        b.size = size;
        b.clear = true;  // zeroed (every particle dead) before its first simulation
        return BufferHandle::make(slot, b.generation);
    }
    // Between a pause and a resume of the pass, as run_culling.
    void simulate_particles(BufferHandle state, const GpuParticles& params) override {
        Buffer* b = lookup(state);
        if (skip_frame_ || shadow_pass_ || !b || !gpu_particle_sim_) return;
        Frame& f = frames_[frame_];
        VkDeviceSize at = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!gpu_particle_set(f, gpu_particle_sim_set_layout_, params, b->buffer, at, set)) return;
        const VkCommandBuffer cmd = f.cmd;
        Texture* resume = target_;
        const bool was_in_pass = in_pass_;
        const f32 viewport[4] = {viewport_[0], viewport_[1], viewport_[2], viewport_[3]};
        end_pass();
        // Earlier sprite draws' reads before these writes.
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (b->clear) {
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
            memory_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            b->clear = false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_particle_sim_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu_particle_sim_layout_, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, (params.spawn[2] + 63) / 64, 1, 1);
        memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        if (!was_in_pass) return;
        if (resume) begin_target_pass(*resume, nullptr);
        else begin_main_pass(nullptr);
        set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
    void draw_gpu_particles(BufferHandle state, const GpuParticles& params, const f32 view_proj[16], TextureHandle texture, bool additive, const f32* soft) override {
        Buffer* b = lookup(state);
        const VkPipeline pipeline = gpu_sprite_pipelines_[additive ? 1 : 0];
        if (skip_frame_ || shadow_pass_ || !b || b->clear || !pipeline || !params.spawn[2]) return;
        Frame& f = frames_[frame_];
        VkDeviceSize at = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!gpu_particle_set(f, gpu_sprite_set_layout_, params, b->buffer, at, set)) return;
        draw_sprites(f, pipeline, gpu_sprite_layout_, params.spawn[2] * 6, view_proj, texture, soft, set, nullptr);
    }
    // A GPU particle set: the parameters (copied into the ring) and the particles.
    bool gpu_particle_set(Frame& f, VkDescriptorSetLayout layout, const GpuParticles& params, VkBuffer particles, VkDeviceSize& at, VkDescriptorSet& set) {
        const VkDeviceSize align = storage_alignment_ ? storage_alignment_ : 256;
        at = (f.ring_used + align - 1) / align * align;
        if (!f.ring_mapped || !f.draw_pool || at + sizeof params > kRingBytes) return false;
        std::memcpy(f.ring_mapped + at, &params, sizeof params);
        f.ring_used = at + sizeof params;
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = f.draw_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo buffers[2] = {{f.ring, at, sizeof params}, {particles, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (u32 k = 0; k < 2; ++k) {
            writes[k].dstSet = set;
            writes[k].dstBinding = k;
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = k ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[k].pBufferInfo = &buffers[k];
        }
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        return true;
    }
    // Sprites (draw_particles from the stream at `stream_at`; draw_gpu_particles from `extra`, set 2): push constants,
    // the texture (set 0), the depth soft particles fade against (set 1).
    void draw_sprites(Frame& f, VkPipeline pipeline, VkPipelineLayout layout, u32 vertex_count, const f32 view_proj[16], TextureHandle texture, const f32* soft,
                      VkDescriptorSet extra, const VkDeviceSize* stream_at) {
        f32 push[kParticlePushBytes / sizeof(f32)] = {};
        std::memcpy(push, view_proj, sizeof(f32) * 16);
        push[16] = lights_.output[0];  // into a scene target: range-compressed
        const Texture* t = lookup_texture(texture);
        if (!t || t->shadow_size) t = lookup_texture(white_);
        if (!t) return;
        const bool soft_fade = soft && soft[0] > 0.0f && in_pass_ && target_ && target_->depth_image;
        if (soft_fade) std::memcpy(push + 20, soft, 4 * sizeof(f32));
        // Set 1: the depth to fade against (white when not soft: the shader does not read it then).
        const Texture* white = lookup_texture(white_);
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = f.draw_pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &particle_depth_layout_;
        VkDescriptorSet sets[2] = {t->set, VK_NULL_HANDLE};
        if (!white || !f.draw_pool || vkAllocateDescriptorSets(device_, &info, &sets[1]) != VK_SUCCESS) return;
        const VkDescriptorImageInfo image{VK_NULL_HANDLE, soft_fade ? target_->depth_view : white->view,
                                          soft_fade ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[1];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        if (soft_fade) target_depth_read_only(true);
        const VkCommandBuffer cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kParticlePushBytes, push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
        if (extra) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &extra, 0, nullptr);
        if (stream_at) vkCmdBindVertexBuffers(cmd, 0, 1, &f.stream, stream_at);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
        if (soft_fade) target_depth_read_only(false);
    }
    // Material Graph code built into the lit pipelines (static and skinned; opaque, translucent, additive),
    // as D3D12's: shaders/mesh.hlsl with the prelude and the code at //@MATERIAL, compiled by the Vulkan
    // SDK's DXC. Without the SDK it is invalid and the built-in material draws (the log says so once).
    MaterialShaderHandle create_material_shader(const char* code) override {
        if (!code || !mesh_layout_) return {};
        u32 slot = 0;
        while (slot < kMaxMaterialShaders && material_shaders_[slot].pipelines[0][0]) ++slot;
        if (slot == kMaxMaterialShaders) return {};
        std::string source = kMeshHlsl;
        source.insert(source.find("//@MATERIAL"), std::string(kMaterialPreludeHlsl) + code);
        std::vector<u32> spirv[2][2];  // [static, skinned][vertex, pixel]
        for (u32 k = 0; k < 2; ++k) {
            for (u32 st = 0; st < 2; ++st) {
                if (!compile_spirv(source, st == 0 ? "vs" : "ps", k == 1 ? "SKINNED" : nullptr, spirv[k][st])) return {};
            }
        }
        MaterialShader& m = material_shaders_[slot];
        const Blend modes[3] = {Blend::Opaque, Blend::MeshTranslucent, Blend::MeshAdditive};
        bool ok = true;
        for (u32 k = 0; k < 2 && ok; ++k) {
            for (u32 b = 0; b < 3 && ok; ++b) {
                ok = make_pipeline(spirv[k][0].data(), spirv[k][0].size() * 4, spirv[k][1].data(), spirv[k][1].size() * 4, k ? kSkinnedAttributes : kMeshAttributes,
                                   k ? 5u : 4u, k ? static_cast<u32>(sizeof(SkinnedVertex)) : static_cast<u32>(sizeof(MeshVertex)), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                   kMeshPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, mesh_layout_, m.pipelines[k][b], false, true, false, modes[b]);
            }
        }
        if (!ok) {
            release_material(m);
            return {};
        }
        return MaterialShaderHandle::make(slot, m.generation & MaterialShaderHandle::kGenerationMask);
    }

    static constexpr u32 kMaxPostShaders = 16;
    struct PostShader {
        VkPipeline pipeline = VK_NULL_HANDLE, pipeline_window = VK_NULL_HANDLE;  // into a target, to the window
        u32 generation = 0;
    };
    PostShaderHandle create_post_shader(const char* code) override {
        if (!code || !post_layout_) return {};
        u32 slot = 0;
        while (slot < kMaxPostShaders && post_shaders_[slot].pipeline) ++slot;
        if (slot == kMaxPostShaders) return {};
        std::string source = std::string("#define LSD_POST_MATERIAL 1\n") + kPostHlsl;
        source.insert(source.find("//@POST"), std::string(code) + "\n");
        std::vector<u32> vs, ps;
        if (!compile_spirv(source, "vs", nullptr, vs, "vs_full", true) || !compile_spirv(source, "ps", nullptr, ps, "ps_composite", true)) return {};
        PostShader& s = post_shaders_[slot];
        auto make = [&](VkPipeline& out) {
            return make_pipeline(vs.data(), vs.size() * 4, ps.data(), ps.size() * 4, nullptr, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, kPostPushBytes,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, post_layout_, out, false, false, false, Blend::Opaque);
        };
        if (!make(s.pipeline) || !window_pipeline([&] { return make(s.pipeline_window); })) {
            if (s.pipeline) vkDestroyPipeline(device_, s.pipeline, nullptr);
            s.pipeline = s.pipeline_window = VK_NULL_HANDLE;
            return {};
        }
        return PostShaderHandle::make(slot, s.generation & PostShaderHandle::kGenerationMask);
    }
    PostShader* lookup_post(PostShaderHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxPostShaders) return nullptr;
        PostShader& s = post_shaders_[handle.index()];
        return s.pipeline && (s.generation & PostShaderHandle::kGenerationMask) == handle.generation() ? &s : nullptr;
    }
    void destroy_post_shader(PostShaderHandle handle) override {
        PostShader* s = lookup_post(handle);
        if (!s) return;
        vkDeviceWaitIdle(device_);  // shortcut: stalls; fine for edits in the editor
        vkDestroyPipeline(device_, s->pipeline, nullptr);
        vkDestroyPipeline(device_, s->pipeline_window, nullptr);
        s->pipeline = s->pipeline_window = VK_NULL_HANDLE;
        ++s->generation;
    }

    void destroy_material_shader(MaterialShaderHandle handle) override {
        MaterialShader* m = lookup_material(handle);
        if (!m) return;
        vkDeviceWaitIdle(device_);  // shortcut: stalls; fine for edits in the editor, defer when materials churn
        release_material(*m);
        ++m->generation;
    }

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

    // GPU timings (stat dump), as D3D12's: a timestamp per slot in this frame, read when the frame is done.
    void gpu_timestamp(u32 slot) override {
        if (skip_frame_ || !timestamp_pool_ || slot >= kGpuTimestamps) return;
        vkCmdWriteTimestamp2(frames_[frame_].cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamp_pool_, frame_ * kGpuTimestamps + slot);
        timestamps_set_[frame_] |= 1u << slot;
    }

    bool gpu_timings(f64 out_ms[kGpuTimestamps]) override {
        if (!have_timings_) return false;
        for (u32 i = 0; i < kGpuTimestamps; ++i) out_ms[i] = gpu_ms_[i];
        return true;
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
        bool clear = false;  // GPU particle state not zeroed yet
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

    struct MaterialShader {
        VkPipeline pipelines[2][3] = {};  // [static, skinned][opaque, translucent, additive]
        u32 generation = 0;
    };

    MaterialShader* lookup_material(MaterialShaderHandle handle) {
        if (!handle.valid() || handle.index() >= kMaxMaterialShaders) return nullptr;
        MaterialShader& m = material_shaders_[handle.index()];
        return m.pipelines[0][0] && (m.generation & MaterialShaderHandle::kGenerationMask) == handle.generation() ? &m : nullptr;
    }

    void release_material(MaterialShader& m) {
        for (auto& kind : m.pipelines) {
            for (VkPipeline& pipeline : kind) {
                if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }
    }

    // The Vulkan SDK's DXC ($VULKAN_SDK/Bin/dxc): only it builds SPIR-V (the Windows SDK's dxc cannot).
    std::filesystem::path dxc_path() {
        const char* sdk = SDL_getenv("VULKAN_SDK");
        if (!sdk || !*sdk) return {};
#if defined(_WIN32)
        const std::filesystem::path dxc = std::filesystem::path(sdk) / "Bin" / "dxc.exe";
#else
        const std::filesystem::path dxc = std::filesystem::path(sdk) / "bin" / "dxc";
#endif
        std::error_code ec;
        return std::filesystem::exists(dxc, ec) ? dxc : std::filesystem::path{};
    }

    // HLSL -> SPIR-V through DXC in a hidden process (editor-time: Material Graph edits and loads).
    // shortcut: a process and temp files per stage (a few hundred ms a material); load dxcompiler in-process if it gets slow
    // `entry` (default <stage>_main) is renamed <stage>_main; `post`: post.hlsl's register shifts (t at 0, s from 16).
    bool compile_spirv(const std::string& source, const char* stage, const char* define, std::vector<u32>& out, const char* entry_name = nullptr, bool post = false) {
        const std::filesystem::path dxc = dxc_path();
        if (dxc.empty()) {
            if (!warned_dxc_) LSD_LOG_INFO("vulkan", "Material Graph materials draw with the built-in material on Vulkan: the Vulkan SDK's DXC compiles them (not installed).");
            warned_dxc_ = true;
            return false;
        }
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) return false;
        const std::string base = "lsd_material_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" + std::to_string(++spirv_jobs_);
        const std::filesystem::path hlsl = dir / (base + ".hlsl"), spv = dir / (base + ".spv"), errors = dir / (base + ".txt");
        {
            std::ofstream file(hlsl, std::ios::binary);
            file << source;
            if (!file) return false;
        }
        const std::string target = std::string(stage) + "_6_0", entry = entry_name ? std::string(entry_name) : std::string(stage) + "_main", dxc_s = dxc.string(), hlsl_s = hlsl.string(),
                          spv_s = spv.string(), errors_s = errors.string(), define_s = define ? std::string("-D") + define : std::string();
        std::vector<const char*> args = {dxc_s.c_str(), "-spirv", "-fspv-target-env=vulkan1.3", "-T", target.c_str(), "-E", entry.c_str(),
                                         "-Fo", spv_s.c_str(), "-Fe", errors_s.c_str()};
        if (define) args.push_back(define_s.c_str());
        const std::string rename = std::string("-fspv-entrypoint-name=") + stage + "_main";
        if (entry_name) args.push_back(rename.c_str());
        if (post) {
            for (const char* a : {"-fvk-t-shift", "0", "0", "-fvk-s-shift", "16", "0"}) args.push_back(a);
        }
        args.push_back(hlsl_s.c_str());
        args.push_back(nullptr);
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);  // no console window
        SDL_Process* process = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (process) {
            SDL_WaitProcess(process, true, nullptr);
            SDL_DestroyProcess(process);
        } else {
            LSD_LOG_ERROR("vulkan", "Could not run %s: %s", dxc_s.c_str(), SDL_GetError());
        }
        out.clear();
        if (std::ifstream file{spv, std::ios::binary | std::ios::ate}) {
            const std::streamoff size = file.tellg();
            if (size > 0 && size % 4 == 0) {
                out.resize(static_cast<usize>(size) / 4);
                file.seekg(0);
                file.read(reinterpret_cast<char*>(out.data()), size);
            }
        }
        if (out.empty()) {  // a compile error: DXC wrote why
            std::ifstream file(errors, std::ios::binary);
            std::string why((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (why.size() > 2000) why.resize(2000);
            LSD_LOG_ERROR("vulkan", "Material Graph shader (%s%s%s) did not compile:\n%s", stage, define ? " " : "", define ? define : "", why.c_str());
        }
        std::filesystem::remove(hlsl, ec);
        std::filesystem::remove(spv, ec);
        std::filesystem::remove(errors, ec);
        return !out.empty();
    }

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
        const u32 flat = 0xFFFF8080u;  // RGBA bytes (little-endian): a normal map's "straight out"
        const void* flat_mip = &flat;
        flat_normal_ = create_texture(1, 1, 1, TextureFormat::RGBA8, &flat_mip);
        no_shadow_ = create_shadow_map(1);  // bound for a view without sun or local shadows (all lit)
        return white_.valid() && flat_normal_.valid() && no_shadow_.valid();
    }

    u32 memory_type(u32 allowed, VkMemoryPropertyFlags wanted) const {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(gpu_.handle, &props);
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((allowed & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        }
        return UINT32_MAX;
    }

    static constexpr u32 kMaterialMemo = 64;  // material sets a frame remembers (draws repeat them)
    struct MaterialMemo {
        VkImageView views[4] = {};
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
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
        VkDescriptorPool draw_pool = VK_NULL_HANDLE;  // the lit meshes' material / view sets, reset each frame
        VkDescriptorSet buffers_set = VK_NULL_HANDLE; // mesh.hlsl's set 2 over this frame's ring
        VkDescriptorSet culled_set = VK_NULL_HANDLE;  // the same, its rows from the cull output (gpu_cull draws)
        MaterialMemo memo[kMaterialMemo];
        u32 memo_count = 0;
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

    void read_timestamps() {
        const u32 set = timestamps_set_[frame_];
        timestamps_set_[frame_] = 0;
        if (!timestamp_pool_ || !(set & 1u)) return;  // slot 0 is where the others count from
        u64 ticks[kGpuTimestamps] = {};
        for (u32 i = 0; i < kGpuTimestamps; ++i) {
            if (!((set >> i) & 1u)) continue;
            if (vkGetQueryPoolResults(device_, timestamp_pool_, frame_ * kGpuTimestamps + i, 1, sizeof(u64), &ticks[i], sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
            ticks[i] &= timestamp_mask_;
        }
        for (u32 i = 0; i < kGpuTimestamps; ++i) {
            gpu_ms_[i] = (set >> i) & 1u ? static_cast<f64>((ticks[i] - ticks[0]) & timestamp_mask_) * static_cast<f64>(timestamp_period_) / 1.0e6 : -1.0;
        }
        have_timings_ = true;
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
        storage_alignment_ = std::max(props.limits.minStorageBufferOffsetAlignment, props.limits.minUniformBufferOffsetAlignment);
        u32 family_count = 0;  // timestamps: when the graphics queue keeps them (stat dump's GPU times)
        vkGetPhysicalDeviceQueueFamilyProperties(gpu_.handle, &family_count, nullptr);
        VkQueueFamilyProperties families[32];
        family_count = std::min(family_count, 32u);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu_.handle, &family_count, families);
        const u32 bits = gpu_.queue_family < family_count ? families[gpu_.queue_family].timestampValidBits : 0;
        if (bits && props.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = kFramesInFlight * kGpuTimestamps;
            if (check(vkCreateQueryPool(device_, &query, nullptr, &timestamp_pool_), "vkCreateQueryPool")) {
                timestamp_period_ = props.limits.timestampPeriod;
                timestamp_mask_ = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
            }
        }
        for (Frame& f : frames_) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = kRingBytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
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
        fill_non_solid_ = supported.fillModeNonSolid == VK_TRUE;  // the Wireframe view mode's line fill
        features.features.fillModeNonSolid = supported.fillModeNonSolid;
        bc_ = supported.textureCompressionBC == VK_TRUE;  // BC1 / BC3 textures (every desktop GPU)
        features.features.textureCompressionBC = supported.textureCompressionBC;

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
                       bool textured = false, bool depth_test = true, bool shadow = false, Blend blend_mode = Blend::Opaque,
                       VkPolygonMode polygon = VK_POLYGON_MODE_FILL) {
        if (!layout_out) {  // pipelines sharing a layout (every mesh and shadow pipeline) make it once
            VkPushConstantRange push{push_stages, 0, push_bytes};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &push;
            if (textured) {  // UI and particles: their texture (set 0)
                layout.setLayoutCount = 1;
                layout.pSetLayouts = &texture_set_layout_;
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
        raster.polygonMode = polygon;
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
        depth.depthTestEnable = depth_test && blend_mode != Blend::MeshOverdraw ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depth_test && blend_mode == Blend::Opaque ? VK_TRUE : VK_FALSE;  // blended sprites never hide each other
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        if (blend_mode != Blend::Opaque) {
            const bool mesh = blend_mode == Blend::MeshTranslucent || blend_mode == Blend::MeshAdditive || blend_mode == Blend::MeshOverdraw;
            const bool adds = blend_mode == Blend::Additive || blend_mode == Blend::MeshAdditive || blend_mode == Blend::MeshOverdraw;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = blend_mode == Blend::Premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = adds ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            // Meshes: the scene target's alpha is mirror-likeness (SSR); translucent surfaces cover it by their opacity, additive ones leave it.
            blend_attachment.srcAlphaBlendFactor = mesh ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = mesh && adds ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
        rendering.pColorAttachmentFormats = &pipeline_format_;  // render targets, unless a window pipeline is being made
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

    // The lit meshes' descriptor set layouts (mesh.hlsl's VK_BIND): 0 the material (t0-t2, s0, t8), 1 the
    // view (t3-t7, s1 compare, s2 clamped), 2 the Lights / rows / material parameter blocks (dynamic
    // uniform buffers over the frame's ring); a descriptor pool per frame and each frame's set 2.
    bool create_mesh_layouts() {
        const VkShaderStageFlags fragment = VK_SHADER_STAGE_FRAGMENT_BIT, both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding material[5] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                          {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                          {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                          {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, fragment, &sampler_},
                                                          {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr}};  // t8 emissive
        const VkDescriptorSetLayoutBinding view[7] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                      {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                      {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                      {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                      {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, fragment, nullptr},
                                                      {5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, fragment, &shadow_sampler_},
                                                      {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, fragment, &post_sampler_}};
        const VkDescriptorSetLayoutBinding buffers[3] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, nullptr},
                                                         {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, nullptr},
                                                         {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, nullptr}};
        const struct {
            const VkDescriptorSetLayoutBinding* bindings;
            u32 count;
            VkDescriptorSetLayout* out;
        } layouts[3] = {{material, 5, &material_set_layout_}, {view, 7, &view_set_layout_}, {buffers, 3, &buffers_set_layout_}};
        for (const auto& l : layouts) {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = l.count;
            info.pBindings = l.bindings;
            if (!check(vkCreateDescriptorSetLayout(device_, &info, nullptr, l.out), "vkCreateDescriptorSetLayout (mesh)")) return false;
        }
        const VkDescriptorPoolSize draw_sizes[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDrawSetsPerFrame * 5 + kCullSetsPerFrame}, {VK_DESCRIPTOR_TYPE_SAMPLER, kDrawSetsPerFrame * 2},
                                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kCullSetsPerFrame * 3 + kComputeSetsPerFrame * 4},
                                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kComputeSetsPerFrame}};
        VkDescriptorPoolCreateInfo draw_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        draw_pool.maxSets = kDrawSetsPerFrame;
        draw_pool.poolSizeCount = 4;
        draw_pool.pPoolSizes = draw_sizes;
        const VkDescriptorPoolSize buffer_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFramesInFlight * 6};
        VkDescriptorPoolCreateInfo buffer_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        buffer_pool.maxSets = kFramesInFlight * 2;  // set 2 over the ring, and its culled twin (rows from the cull output)
        buffer_pool.poolSizeCount = 1;
        buffer_pool.pPoolSizes = &buffer_size;
        if (!check(vkCreateDescriptorPool(device_, &buffer_pool, nullptr, &buffers_pool_), "vkCreateDescriptorPool (mesh buffers)")) return false;
        for (Frame& f : frames_) {
            if (!check(vkCreateDescriptorPool(device_, &draw_pool, nullptr, &f.draw_pool), "vkCreateDescriptorPool (mesh)")) return false;
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = buffers_pool_;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &buffers_set_layout_;
            if (!check(vkAllocateDescriptorSets(device_, &info, &f.buffers_set), "vkAllocateDescriptorSets (mesh buffers)")) return false;
            const VkDescriptorBufferInfo ranges[3] = {{f.ring, 0, sizeof(LightSetup)}, {f.ring, 0, kRowsBytes}, {f.ring, 0, kParamsBytes}};
            VkWriteDescriptorSet writes[3] = {};
            for (u32 i = 0; i < 3; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = f.buffers_set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[i].pBufferInfo = &ranges[i];
            }
            vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
        }
        const VkPushConstantRange push{both, 0, kMeshPushBytes};
        VkPipelineLayoutCreateInfo mesh{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout mesh_sets[3] = {material_set_layout_, view_set_layout_, buffers_set_layout_};
        mesh.setLayoutCount = 3;
        mesh.pSetLayouts = mesh_sets;
        mesh.pushConstantRangeCount = 1;
        mesh.pPushConstantRanges = &push;
        if (!check(vkCreatePipelineLayout(device_, &mesh, nullptr, &mesh_layout_), "vkCreatePipelineLayout (mesh)")) return false;
        // Shadow casters (shadow.hlsl): the Masked cut-out's texture (set 0) and the rows (set 1, storage).
        const VkPushConstantRange shadow_push{both, 0, kShadowPushBytes};
        const VkDescriptorSetLayout shadow_sets[2] = {texture_set_layout_, rows_set_layout_};
        VkPipelineLayoutCreateInfo shadow{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        shadow.setLayoutCount = 2;
        shadow.pSetLayouts = shadow_sets;
        shadow.pushConstantRangeCount = 1;
        shadow.pPushConstantRanges = &shadow_push;
        return check(vkCreatePipelineLayout(device_, &shadow, nullptr, &shadow_layout_), "vkCreatePipelineLayout (shadow)");
    }

    bool create_scene_pipelines() {
#if LSD_VULKAN_HAVE_SPIRV
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_.handle, &props);
        if (props.limits.maxPushConstantsSize < kMeshPushBytes || props.limits.maxUniformBufferRange < sizeof(LightSetup)) {
            LSD_LOG_WARN("vulkan", "This GPU offers %u bytes of push constants and %u-byte uniform buffers (meshes need %u and %u): meshes are not drawn.",
                         props.limits.maxPushConstantsSize, props.limits.maxUniformBufferRange, kMeshPushBytes, static_cast<u32>(sizeof(LightSetup)));
            return true;
        }
        if (!create_texture_support() || !create_rings() || !create_post_pipelines() || !create_mesh_layouts()) return false;
        if (!create_culling()) LSD_LOG_WARN("vulkan", "GPU culling is off: its compute passes could not be made (gpu_cull draws draw every copy).");
        if (!create_morphs()) morph_pipeline_ = VK_NULL_HANDLE;  // optional: morphs then mix on the CPU
        const auto& skinned_attributes = kSkinnedAttributes;
        const auto& mesh_attributes = kMeshAttributes;
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
        // The lit meshes: [static, skinned, instanced] x [opaque, wireframe, translucent, additive, overdraw], as D3D12's.
        struct Kind {
            const u32 *vs, *ps;
            usize vs_size, ps_size;
            const VkVertexInputAttributeDescription* attributes;
            u32 attribute_count, stride;
        };
        const Kind kinds[3] = {{kMeshVsSpirv, kMeshPsSpirv, sizeof kMeshVsSpirv, sizeof kMeshPsSpirv, mesh_attributes, 4, sizeof(MeshVertex)},
                               {kMeshSkinnedVsSpirv, kMeshSkinnedPsSpirv, sizeof kMeshSkinnedVsSpirv, sizeof kMeshSkinnedPsSpirv, skinned_attributes, 5, sizeof(SkinnedVertex)},
                               {kMeshInstancedVsSpirv, kMeshInstancedPsSpirv, sizeof kMeshInstancedVsSpirv, sizeof kMeshInstancedPsSpirv, mesh_attributes, 3, sizeof(MeshVertex)}};  // copies are not lightmapped: no lightmap UV
        const Blend modes[5] = {Blend::Opaque, Blend::Opaque, Blend::MeshTranslucent, Blend::MeshAdditive, Blend::MeshOverdraw};
        for (u32 k = 0; k < 3; ++k) {
            for (u32 m = 0; m < 5; ++m) {
                const VkPolygonMode polygon = m == 1 && fill_non_solid_ ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                if (!make_pipeline(kinds[k].vs, kinds[k].vs_size, kinds[k].ps, kinds[k].ps_size, kinds[k].attributes, kinds[k].attribute_count, kinds[k].stride, triangles,
                                   kMeshPushBytes, stages, mesh_layout_, mesh_pipelines_[k][m], false, true, false, modes[m], polygon)) return false;
            }
        }
        if (!particle_layout_) {  // the sprite (set 0) and the depth soft particles fade against (set 1)
            const VkDescriptorSetLayoutBinding depth_binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo depth_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            depth_info.bindingCount = 1;
            depth_info.pBindings = &depth_binding;
            if (!check(vkCreateDescriptorSetLayout(device_, &depth_info, nullptr, &particle_depth_layout_), "vkCreateDescriptorSetLayout (particle depth)")) return false;
            const VkPushConstantRange push{stages, 0, kParticlePushBytes};
            const VkDescriptorSetLayout sets[2] = {texture_set_layout_, particle_depth_layout_};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 2;
            layout.pSetLayouts = sets;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &push;
            if (!check(vkCreatePipelineLayout(device_, &layout, nullptr, &particle_layout_), "vkCreatePipelineLayout (particles)")) return false;
        }
        return make_pipeline(kSkyVsSpirv, sizeof kSkyVsSpirv, kSkyPsSpirv, sizeof kSkyPsSpirv, nullptr, 0, 0, triangles, kMeshPushBytes, stages, mesh_layout_,
                             sky_pipeline_, false, false) &&  // behind everything: drawn first, depth untouched
               make_pipeline(kShadowVsSpirv, sizeof kShadowVsSpirv, nullptr, 0, shadow_attributes, 2, sizeof(MeshVertex), triangles, kShadowPushBytes, stages,
                             shadow_layout_, shadow_pipelines_[0], false, true, true) &&
               make_pipeline(kShadowSkinnedVsSpirv, sizeof kShadowSkinnedVsSpirv, nullptr, 0, shadow_skinned_attributes, 4, sizeof(SkinnedVertex), triangles,
                             kShadowPushBytes, stages, shadow_layout_, shadow_pipelines_[1], false, true, true) &&
               make_pipeline(kShadowInstancedVsSpirv, sizeof kShadowInstancedVsSpirv, nullptr, 0, shadow_attributes, 2, sizeof(MeshVertex), triangles,
                             kShadowPushBytes, stages, shadow_layout_, shadow_pipelines_[2], false, true, true) &&
               make_pipeline(kShadowVsSpirv, sizeof kShadowVsSpirv, kShadowPsSpirv, sizeof kShadowPsSpirv, shadow_attributes, 2, sizeof(MeshVertex), triangles,
                             kShadowPushBytes, stages, shadow_layout_, shadow_pipelines_[3], false, true, true) &&
               make_pipeline(kUiVsSpirv, sizeof kUiVsSpirv, kUiPsSpirv, sizeof kUiPsSpirv, ui_attributes, 3, sizeof(UiVertex), triangles, kUiPushBytes, stages,
                             ui_layout_, ui_target_pipeline_, true, false, false, Blend::Premultiplied) &&
               window_pipeline([&] {
                   return make_pipeline(kUiVsSpirv, sizeof kUiVsSpirv, kUiPsSpirv, sizeof kUiPsSpirv, ui_attributes, 3, sizeof(UiVertex), triangles, kUiPushBytes, stages,
                                        ui_layout_, ui_pipeline_, true, false, false, Blend::Premultiplied);
               }) &&
               make_pipeline(kParticleVsSpirv, sizeof kParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, particle_attributes, 3, sizeof(ParticleVertex),
                             triangles, kParticlePushBytes, stages, particle_layout_, particle_pipelines_[0], true, true, false, Blend::Alpha) &&
               make_pipeline(kParticleVsSpirv, sizeof kParticleVsSpirv, kParticlePsSpirv, sizeof kParticlePsSpirv, particle_attributes, 3, sizeof(ParticleVertex),
                             triangles, kParticlePushBytes, stages, particle_layout_, particle_pipelines_[1], true, true, false, Blend::Additive) &&
               (create_gpu_particles(stages, triangles) || true) &&  // optional: GPU emitters then run on the CPU
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
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE, line_layout_ = VK_NULL_HANDLE, shadow_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipelines_[3][5] = {};  // [static, skinned, instanced][opaque, wireframe, translucent, additive, overdraw]
    VkPipeline line_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE, view_set_layout_ = VK_NULL_HANDLE, buffers_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool buffers_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet view_set_ = VK_NULL_HANDLE;  // this frame's set 1 (made again when a shadow map or atlas changes)
    LightSetup lights_ = default_lights();
    bool lights_in_ring_ = false;  // written into this frame's ring since the last set_lights
    u32 lights_at_ = 0;
    TextureHandle local_shadow_, decal_atlas_, reflection_atlas_, lightmap_atlas_, flat_normal_;
    bool fill_non_solid_ = false;
    bool bc_ = false;  // textureCompressionBC: BC1 / BC3 textures
    bool warned_sets_ = false;
    MaterialShader material_shaders_[kMaxMaterialShaders];
    u32 spirv_jobs_ = 0;
    VkDescriptorSetLayout cull_set_layout_ = VK_NULL_HANDLE;
    // The Hi-Z (build_hiz): R32F in GENERAL, its views (all mips; each mip), per-frame sets per mip, its view-projection.
    VkDescriptorSetLayout hiz_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout hiz_layout_ = VK_NULL_HANDLE;
    VkPipeline hiz_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool hiz_pool_ = VK_NULL_HANDLE;
    VkImage hiz_image_ = VK_NULL_HANDLE;
    VkDeviceMemory hiz_memory_ = VK_NULL_HANDLE;
    VkImageView hiz_view_ = VK_NULL_HANDLE, hiz_mip_views_[kMaxHizMips] = {};
    VkDescriptorSet hiz_sets_[kFramesInFlight][kMaxHizMips] = {};
    u32 hiz_size_[2] = {}, hiz_mips_ = 0;
    f32 hiz_view_proj_[16] = {};
    bool hiz_valid_ = false;
    VkPipelineLayout cull_layout_ = VK_NULL_HANDLE;
    VkPipeline cull_pipelines_[3] = {};  // reset, cull, arguments
    VkDescriptorSetLayout gpu_particle_sim_set_layout_ = VK_NULL_HANDLE, gpu_sprite_set_layout_ = VK_NULL_HANDLE;  // GPU particles
    VkPipelineLayout gpu_particle_sim_layout_ = VK_NULL_HANDLE, gpu_sprite_layout_ = VK_NULL_HANDLE;
    VkPipeline gpu_particle_sim_ = VK_NULL_HANDLE, gpu_sprite_pipelines_[2] = {};  // the pass; alpha, additive sprites
    VkDescriptorSetLayout morph_set_layout_ = VK_NULL_HANDLE;  // morph targets (create_morphs)
    VkPipelineLayout morph_layout_ = VK_NULL_HANDLE;
    VkPipeline morph_pipeline_ = VK_NULL_HANDLE;
    std::vector<MorphJob> pending_morphs_;
    Buffer cull_out_, cull_args_;        // the visible rows; the count + one indirect draw per batch
    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;  // kGpuTimestamps per frame in flight
    u32 timestamps_set_[kFramesInFlight] = {};
    f64 gpu_ms_[kGpuTimestamps] = {};
    f32 timestamp_period_ = 0.0f;  // nanoseconds a tick
    u64 timestamp_mask_ = 0;
    bool have_timings_ = false;
    bool warned_dxc_ = false;
    TextureHandle sun_shadow_, no_shadow_;
    Texture* shadow_pass_ = nullptr;   // the shadow map being drawn (between begin / end_shadow_pass)
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipelines_[4] = {};  // static, skinned, instanced, Masked
    bool depth_clamp_ = false;
    f32 viewport_[4] = {};  // the last set_viewport (draw_ui puts it back)
    VkPipelineLayout ui_layout_ = VK_NULL_HANDLE, particle_layout_ = VK_NULL_HANDLE;
    VkPipeline ui_pipeline_ = VK_NULL_HANDLE, particle_pipelines_[2] = {};  // alpha, additive
    VkPipeline ui_target_pipeline_ = VK_NULL_HANDLE, post_window_[2] = {};  // the UI into a target; composite and FXAA to the window
    VkFormat pipeline_format_ = kTargetFormat;  // what make_pipeline builds for (window_pipeline: the swapchain's)
    bool in_target_ = false;                    // a render target is bound (not the window)
    template <class Make>
    bool window_pipeline(Make make) {  // `make` builds for the window's format
        pipeline_format_ = format_.format;
        const bool ok = make();
        pipeline_format_ = kTargetFormat;
        return ok;
    }
    bool warned_stream_ = false;
    bool in_pass_ = false;               // between vkCmdBeginRendering and vkCmdEndRendering
    Texture* target_ = nullptr;          // the render target being drawn into (null: the window or a shadow map)
    Texture* resume_target_ = nullptr;   // the target a shadow pass interrupted
    VkSampler post_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    PostShader post_shaders_[kMaxPostShaders];
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
    VkDescriptorSetLayout particle_depth_layout_ = VK_NULL_HANDLE;  // particle.hlsl's set 1
    VkDescriptorPool texture_pool_ = VK_NULL_HANDLE;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rows_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool rows_pool_ = VK_NULL_HANDLE;
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
