// LSD_Engine editor entry point: editor shell (Viewport, Outliner, Details, Content Browser).
#include "editor_ui.h"

#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/render/grid.h"
#include "lsd/render/mesh.h"
#include "lsd/rhi/rhi.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

void to_rgba(const lsd::LinearColor& c, lsd::f32 out[4]) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

lsd::render::GridColors grid_colors(const lsd::editor::EditorTheme& t) {
    lsd::render::GridColors colors;
    to_rgba(t.grid_minor, colors.minor);
    to_rgba(t.grid_major, colors.major);
    to_rgba(t.axis_x, colors.axis_x);
    to_rgba(t.axis_y, colors.axis_y);
    return colors;
}

// Built-in scene until levels exist (milestone 3); each object is one Outliner row.
struct SampleObject {
    const lsd::render::GpuMesh* mesh;
    lsd::Vec3 position;
    lsd::f32 color[4];
    int outliner_row;
};

void draw_mesh(lsd::rhi::Device& device, const lsd::Mat4& view_proj, const SampleObject& o, bool selected) {
    lsd::rhi::MeshDraw draw{};
    draw.vertices = o.mesh->vertices;
    draw.indices = o.mesh->indices;
    draw.index_count = o.mesh->index_count;
    const lsd::Mat4 world = lsd::translation(o.position);
    const lsd::Mat4 mvp = view_proj * world;
    for (int i = 0; i < 16; ++i) draw.mvp[i] = mvp.m[i];
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) draw.world_rotation[c * 4 + r] = world.at(r, c);
        draw.world_rotation[c * 4 + 3] = 0.0f;
    }
    for (int i = 0; i < 4; ++i) draw.color[i] = o.color[i];
    if (selected) {  // shortcut: selection tint until the outline post-process exists
        draw.color[0] = 1.0f;
        draw.color[1] = 0.85f;
        draw.color[2] = 0.25f;
    }
    device.draw_mesh(draw);
}

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
    long screenshot_frame = -1;  // -screenshot=<n>: save frame n to Saved/screenshot.bmp (OpenGL)
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-gpu=", 5) == 0) desc.adapter_override = std::atoi(argv[i] + 5);
        else if (std::strcmp(argv[i], "-rhi=d3d12") == 0) desc.backend = lsd::rhi::Backend::D3D12;
        else if (std::strcmp(argv[i], "-rhi=vulkan") == 0) desc.backend = lsd::rhi::Backend::Vulkan;
        else if (std::strcmp(argv[i], "-rhi=opengl") == 0) desc.backend = lsd::rhi::Backend::OpenGL;
        else if (std::strcmp(argv[i], "-validation") == 0) desc.enable_validation = true;
        else if (std::strncmp(argv[i], "-frames=", 8) == 0) max_frames = std::atol(argv[i] + 8);
        else if (std::strncmp(argv[i], "-screenshot=", 12) == 0) screenshot_frame = std::atol(argv[i] + 12);
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
    std::filesystem::create_directories("Content", ignored);  // the project's assets, like Unreal's Content/

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

    lsd::render::MeshData cube_data = lsd::render::make_cube(allocator);
    lsd::render::MeshData sphere_data = lsd::render::make_sphere(allocator);
    lsd::render::GpuMesh cube = lsd::render::upload_mesh(*device, cube_data);
    lsd::render::GpuMesh sphere = lsd::render::upload_mesh(*device, sphere_data);
    lsd::render::free_mesh(allocator, cube_data);
    lsd::render::free_mesh(allocator, sphere_data);
    const SampleObject scene[] = {
        {&cube, {0.0f, 0.0f, 0.0f}, {0.7f, 0.7f, 0.72f, 1.0f}, 0},
        {&sphere, {1.5f, 1.5f, 0.0f}, {0.85f, 0.35f, 0.3f, 1.0f}, 1},
    };
    const lsd::editor::OutlineRow outline_rows[] = {
        {"Cube", lsd::editor::RowKind::Mesh, 0},
        {"Sphere", lsd::editor::RowKind::Mesh, 0},
    };
    state.rows = outline_rows;
    state.row_count = 2;
    lsd::render::GridColors current_grid = grid_colors(state.theme);
    lsd::render::GpuLines grid = lsd::render::create_grid(*device, allocator, current_grid);

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

        // Rebuild the grid when its theme colours change.
        const lsd::render::GridColors wanted_grid = grid_colors(state.theme);
        if (std::memcmp(&wanted_grid, &current_grid, sizeof wanted_grid) != 0) {
            lsd::render::destroy_lines(*device, grid);
            grid = lsd::render::create_grid(*device, allocator, wanted_grid);
            current_grid = wanted_grid;
        }

        const lsd::LinearColor& bg = state.theme.viewport_background;
        const lsd::f32 clear_color[4] = {bg.r, bg.g, bg.b, 1.0f};
        device->begin_frame(clear_color);
        const lsd::f32* rect = state.viewport_rect;
        if (rect[2] >= 8.0f && rect[3] >= 8.0f) {
            // ImGui works in window points; the swapchain is in pixels (differs on high-DPI screens).
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            device->set_viewport(rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y);
            const lsd::Mat4 view_proj = lsd::editor::camera_projection(state.camera, rect[2] / rect[3]) * lsd::editor::camera_view(state.camera);
            if (state.show_grid) device->draw_lines(grid.vertices, grid.vertex_count, view_proj.m);
            for (const SampleObject& o : scene) draw_mesh(*device, view_proj, o, state.selected_row == o.outliner_row);
        }
        device->render_debug_ui();
        if (frame == screenshot_frame && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl for now.");
        }
        device->end_frame();
        ++frame;
    }

    lsd::render::destroy_lines(*device, grid);
    lsd::render::destroy_mesh(*device, cube);
    lsd::render::destroy_mesh(*device, sphere);
    lsd::rhi::destroy_device(device, allocator);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
