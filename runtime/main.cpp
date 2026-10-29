// LSD_Engine runtime entry point.
// Prints the hardware report, starts the job system, opens a window and draws a test triangle.
#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    lsd::rhi::DeviceDesc desc{};
    long max_frames = -1;  // -frames=<n>: quit after n frames (smoke tests)
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-gpu=", 5) == 0) desc.adapter_override = std::atoi(argv[i] + 5);
        else if (std::strcmp(argv[i], "-rhi=d3d12") == 0) desc.backend = lsd::rhi::Backend::D3D12;
        else if (std::strcmp(argv[i], "-rhi=vulkan") == 0) desc.backend = lsd::rhi::Backend::Vulkan;
        else if (std::strcmp(argv[i], "-rhi=opengl") == 0) desc.backend = lsd::rhi::Backend::OpenGL;
        else if (std::strcmp(argv[i], "-validation") == 0) desc.enable_validation = true;
        else if (std::strncmp(argv[i], "-frames=", 8) == 0) max_frames = std::atol(argv[i] + 8);
        else LSD_LOG_WARN("engine", "Unknown argument: %s", argv[i]);
    }

    LSD_LOG_INFO("engine", "LSD_Engine 0.1.0");
    const lsd::SystemInfo info = lsd::query_system_info();
    lsd::log_system_info(info);

    lsd::Allocator& allocator = lsd::default_allocator();
    lsd::JobSystem jobs(allocator, lsd::recommended_worker_count(info));
    LSD_LOG_INFO("engine", "Job system: %u worker threads", jobs.worker_count());

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LSD_LOG_ERROR("engine", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    int width = 1280, height = 720;
    SDL_Window* window = SDL_CreateWindow("LSD_Engine", width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        LSD_LOG_ERROR("engine", "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GetWindowSizeInPixels(window, &width, &height);

    lsd::rhi::Device* device = lsd::rhi::create_device(desc, allocator);
    void* native_window = nullptr;
#if defined(LSD_PLATFORM_WINDOWS)
    native_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    if (!device || !device->create_swapchain(native_window, static_cast<lsd::u32>(width), static_cast<lsd::u32>(height))) {
        LSD_LOG_ERROR("engine", "Could not start the graphics device.");
        lsd::rhi::destroy_device(device, allocator);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const lsd::rhi::AdapterInfo& adapter = device->adapter();
    const lsd::rhi::MemoryBudget budget = device->query_memory_budget();
    LSD_LOG_INFO("engine", "Graphics: %s on %s", lsd::rhi::to_string(device->backend()), adapter.name);
    LSD_LOG_INFO("engine", "VRAM budget: %llu MiB (using %llu MiB)",
                 static_cast<unsigned long long>(budget.budget_bytes / lsd::MiB),
                 static_cast<unsigned long long>(budget.usage_bytes / lsd::MiB));
    LSD_LOG_INFO("engine", "Quality preset: %s", lsd::rhi::to_string(lsd::rhi::recommend_preset(adapter, info.total_ram_bytes)));

    const lsd::f32 clear_color[4] = {0.051f, 0.016f, 0.016f, 1.0f};  // editor viewport #0D0404
    bool running = true;
    bool minimized = false;
    for (long frame = 0; running && frame != max_frames;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; break;
                case SDL_EVENT_WINDOW_RESTORED: minimized = false; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    device->resize_swapchain(static_cast<lsd::u32>(event.window.data1), static_cast<lsd::u32>(event.window.data2));
                    break;
                default: break;
            }
        }
        if (minimized) { SDL_Delay(16); continue; }
        device->begin_frame(clear_color);
        device->draw_test_triangle();
        device->end_frame();
        ++frame;
    }

    lsd::rhi::destroy_device(device, allocator);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
