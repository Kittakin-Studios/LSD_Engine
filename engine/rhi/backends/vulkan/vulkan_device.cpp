// Vulkan 1.3 backend: adapter enumeration, VK_EXT_memory_budget, swapchain with dynamic rendering,
// test triangle (SPIR-V compiled from HLSL by DXC at build time) and ImGui.
#include "lsd/rhi/rhi.h"
#include "lsd/foundation/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <cstring>
#include <new>

#if LSD_VULKAN_HAVE_SPIRV
#include "triangle_vs.spv.h"
#include "triangle_ps.spv.h"
#endif

namespace lsd::rhi {

namespace {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxAdapters = 16;
constexpr u32 kMaxSwapchainImages = 8;

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
        if (debug_ui_) ImGui_ImplVulkan_Shutdown();
        if (device_) {
            destroy_swapchain();
            for (u32 i = 0; i < kFramesInFlight; ++i) {
                vkDestroySemaphore(device_, frames_[i].image_available, nullptr);
                vkDestroyFence(device_, frames_[i].fence, nullptr);
                vkDestroyCommandPool(device_, frames_[i].pool, nullptr);
            }
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
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
        return build_swapchain(width, height) && create_triangle_pipeline();
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

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views_[image_];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(color.clearValue.color.float32, clear_color, sizeof(f32) * 4);
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent_;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(f.cmd, &rendering);

        // Negative height flips Y so clip space matches D3D12 and OpenGL (+Y up).
        const VkViewport viewport{0.0f, static_cast<f32>(extent_.height), static_cast<f32>(extent_.width), -static_cast<f32>(extent_.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(f.cmd, 0, 1, &viewport);
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    }

    void draw_test_triangle(f32 scale, f32 rotation_radians) override {
        if (skip_frame_ || !pipeline_) return;
        const VkCommandBuffer cmd = frames_[frame_].cmd;
        const f32 params[3] = {scale, rotation_radians, extent_.height ? static_cast<f32>(extent_.width) / static_cast<f32>(extent_.height) : 1.0f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof params, params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    void end_frame() override {
        if (skip_frame_) return;
        Frame& f = frames_[frame_];
        vkCmdEndRendering(f.cmd);
        transition(f.cmd, images_[image_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
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
        init.DescriptorPoolSize = 64;  // backend creates its own pool
        init.MinImageCount = 2;
        init.ImageCount = image_count_;
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_.format;
        debug_ui_ = ImGui_ImplVulkan_Init(&init);
        return debug_ui_;
    }

    void debug_ui_new_frame() override { ImGui_ImplVulkan_NewFrame(); }

    void render_debug_ui() override {
        if (!skip_frame_) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frames_[frame_].cmd);
    }

private:
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
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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

    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
