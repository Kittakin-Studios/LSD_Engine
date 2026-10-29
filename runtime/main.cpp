// LSD_Engine runtime entry point.
// Prints the hardware report, starts the job system, opens a window and draws a test triangle
// with a debug Details panel (Dear ImGui) generated from reflection.
#include "details_panel.h"
#include "viewport_settings.h"

#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

constexpr char kSavedDir[] = "Saved";
constexpr char kSettingsPath[] = "Saved/viewport_settings.json";
constexpr char kImGuiIniPath[] = "Saved/imgui.ini";

bool save_settings(const lsd::ViewportSettings& settings) {
    char text[4096];
    const lsd::usize length = lsd::reflect::save_text(settings, text, sizeof text);
    if (length >= sizeof text) return false;
    FILE* file = std::fopen(kSettingsPath, "wb");
    if (!file) return false;
    const bool ok = std::fwrite(text, 1, length, file) == length;
    return std::fclose(file) == 0 && ok;
}

bool load_settings(lsd::ViewportSettings& settings) {
    FILE* file = std::fopen(kSettingsPath, "rb");
    if (!file) return false;
    char text[4096];
    const lsd::usize length = std::fread(text, 1, sizeof text, file);
    std::fclose(file);
    lsd::ViewportSettings loaded = settings;  // load into a copy so a bad file changes nothing
    if (length == sizeof text || !lsd::reflect::load_text(loaded, text, length)) return false;
    settings = loaded;
    return true;
}

lsd::u64 window_flags_for(lsd::rhi::Backend backend) {
    switch (backend) {
        case lsd::rhi::Backend::OpenGL: return SDL_WINDOW_OPENGL;
        case lsd::rhi::Backend::Vulkan: return SDL_WINDOW_VULKAN;
        default: return 0;
    }
}

} // namespace

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
    std::error_code ignored;
    std::filesystem::create_directories(kSavedDir, ignored);

    // The device comes first: the window needs backend-specific flags (OpenGL, Vulkan).
    lsd::rhi::Device* device = lsd::rhi::create_device(desc, allocator);
    if (!device) {
        LSD_LOG_ERROR("engine", "Could not start the graphics device.");
        SDL_Quit();
        return 1;
    }
    int width = 1280, height = 720;
    SDL_Window* window = SDL_CreateWindow("LSD_Engine", width, height,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | window_flags_for(device->backend()));
    if (window) SDL_GetWindowSizeInPixels(window, &width, &height);
    if (!window || !device->create_swapchain(window, static_cast<lsd::u32>(width), static_cast<lsd::u32>(height))) {
        LSD_LOG_ERROR("engine", "Could not create the window or swapchain: %s", SDL_GetError());
        lsd::rhi::destroy_device(device, allocator);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const lsd::rhi::AdapterInfo& adapter = device->adapter();
    LSD_LOG_INFO("engine", "Graphics: %s on %s", lsd::rhi::to_string(device->backend()), adapter.name);
    const lsd::rhi::MemoryBudget start_budget = device->query_memory_budget();
    LSD_LOG_INFO("engine", "VRAM budget: %llu MiB (using %llu MiB)",
                 static_cast<unsigned long long>(start_budget.budget_bytes / lsd::MiB),
                 static_cast<unsigned long long>(start_budget.usage_bytes / lsd::MiB));
    LSD_LOG_INFO("engine", "Quality preset: %s", lsd::rhi::to_string(lsd::rhi::recommend_preset(adapter, info.total_ram_bytes)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = kImGuiIniPath;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOther(window);
    if (!device->init_debug_ui()) LSD_LOG_WARN("engine", "Debug UI unavailable on this backend.");

    lsd::ViewportSettings settings;
    char status[128] = "";
    if (load_settings(settings)) std::snprintf(status, sizeof status, "Loaded %s", kSettingsPath);

    bool running = true;
    bool minimized = false;
    float angle = 0.0f;
    lsd::u64 last_ticks = SDL_GetTicksNS();
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

        const lsd::u64 now = SDL_GetTicksNS();
        const float dt = static_cast<float>(now - last_ticks) * 1e-9f;
        last_ticks = now;
        angle += settings.spin_speed * dt * 0.0174532925f;  // degrees to radians
        const lsd::rhi::MemoryBudget budget = device->query_memory_budget();
        settings.frame_rate = ImGui::GetIO().Framerate;
        settings.vram_used = budget.usage_bytes / lsd::MiB;
        settings.vram_budget = budget.budget_bytes / lsd::MiB;

        device->debug_ui_new_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextDisabled("%s  |  %s", settings.static_type().name, lsd::rhi::to_string(device->backend()));
            if (ImGui::Button("Save")) {
                std::snprintf(status, sizeof status, save_settings(settings) ? "Saved %s" : "Could not save %s", kSettingsPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                std::snprintf(status, sizeof status, load_settings(settings) ? "Loaded %s" : "Could not load %s", kSettingsPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                settings = lsd::ViewportSettings{};
                std::snprintf(status, sizeof status, "Reset to defaults");
            }
            if (status[0]) ImGui::TextDisabled("%s", status);
            ImGui::Separator();
            lsd::draw_details(lsd::reflect::type_of<lsd::ViewportSettings>(), &settings);
        }
        ImGui::End();
        ImGui::Render();

        const lsd::f32 clear_color[4] = {settings.background.r, settings.background.g, settings.background.b, 1.0f};
        device->begin_frame(clear_color);
        if (settings.draw_triangle) device->draw_test_triangle(settings.scale, angle);
        device->render_debug_ui();
        device->end_frame();
        ++frame;
    }

    lsd::rhi::destroy_device(device, allocator);  // shuts down the backend's ImGui renderer
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
