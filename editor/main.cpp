// LSD_Engine editor entry point: editor shell (Viewport, Outliner, Details, Content Browser).
#include "editor_ui.h"

#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

lsd::u64 window_flags_for(lsd::rhi::Backend backend) {
    switch (backend) {
        case lsd::rhi::Backend::OpenGL: return SDL_WINDOW_OPENGL;
        case lsd::rhi::Backend::Vulkan: return SDL_WINDOW_VULKAN;
        default: return 0;
    }
}

// Segoe UI on Windows reads closer to the Unreal editor than ImGui's built-in pixel font.
void load_editor_font() {
#if defined(LSD_PLATFORM_WINDOWS)
    constexpr char kFont[] = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(kFont)) ImGui::GetIO().Fonts->AddFontFromFileTTF(kFont, 18.0f);
#endif
}

} // namespace

int main(int argc, char** argv) {
    // shortcut: startup mirrors runtime/main.cpp; fold both into a shared app layer when a third exe needs it.
    lsd::rhi::DeviceDesc desc{};
    long max_frames = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-gpu=", 5) == 0) desc.adapter_override = std::atoi(argv[i] + 5);
        else if (std::strcmp(argv[i], "-rhi=d3d12") == 0) desc.backend = lsd::rhi::Backend::D3D12;
        else if (std::strcmp(argv[i], "-rhi=vulkan") == 0) desc.backend = lsd::rhi::Backend::Vulkan;
        else if (std::strcmp(argv[i], "-rhi=opengl") == 0) desc.backend = lsd::rhi::Backend::OpenGL;
        else if (std::strcmp(argv[i], "-validation") == 0) desc.enable_validation = true;
        else if (std::strncmp(argv[i], "-frames=", 8) == 0) max_frames = std::atol(argv[i] + 8);
        else LSD_LOG_WARN("editor", "Unknown argument: %s", argv[i]);
    }

    LSD_LOG_INFO("editor", "LSD_Engine Editor 0.1.0");
    const lsd::SystemInfo info = lsd::query_system_info();
    lsd::Allocator& allocator = lsd::default_allocator();
    lsd::JobSystem jobs(allocator, lsd::recommended_worker_count(info));

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LSD_LOG_ERROR("editor", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    std::error_code ignored;
    std::filesystem::create_directories("Saved", ignored);
    std::filesystem::create_directories("Config", ignored);

    lsd::rhi::Device* device = lsd::rhi::create_device(desc, allocator);
    if (!device) {
        LSD_LOG_ERROR("editor", "Could not start the graphics device.");
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("LSD_Engine Editor", 1600, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED |
                                              window_flags_for(device->backend()));
    int width = 0, height = 0;
    if (window) SDL_GetWindowSizeInPixels(window, &width, &height);
    if (!window || !device->create_swapchain(window, static_cast<lsd::u32>(width), static_cast<lsd::u32>(height))) {
        LSD_LOG_ERROR("editor", "Could not create the window or swapchain: %s", SDL_GetError());
        lsd::rhi::destroy_device(device, allocator);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    LSD_LOG_INFO("editor", "Graphics: %s on %s", lsd::rhi::to_string(device->backend()), device->adapter().name);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "Saved/editor_layout.ini";
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    load_editor_font();
    ImGui_ImplSDL3_InitForOther(window);
    if (!device->init_debug_ui()) LSD_LOG_WARN("editor", "Editor UI unavailable on this backend.");

    lsd::editor::EditorState state;
    lsd::editor::load_theme(state.theme);
    lsd::editor::apply_theme(state.theme);

    bool running = true;
    bool minimized = false;
    for (long frame = 0; running && frame != max_frames;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
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

        device->debug_ui_new_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        lsd::editor::draw_editor(state);
        ImGui::Render();

        const lsd::LinearColor& bg = state.theme.viewport_background;
        const lsd::f32 clear_color[4] = {bg.r, bg.g, bg.b, 1.0f};
        device->begin_frame(clear_color);
        device->render_debug_ui();
        device->end_frame();
        ++frame;
    }

    lsd::rhi::destroy_device(device, allocator);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
