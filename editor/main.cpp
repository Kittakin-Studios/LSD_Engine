// LSD_Engine editor entry point: editor shell (Viewport, Outliner, Details, Content Browser).
#include "editor_scene.h"
#include "editor_ui.h"
#include "thumbnails.h"

#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/render/grid.h"
#include "lsd/render/mesh.h"
#include "lsd/rhi/rhi.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>

namespace {

namespace fs = std::filesystem;

constexpr char kContentDir[] = "Content";
constexpr char kDataDir[] = "DerivedData";

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

void draw_object(lsd::rhi::Device& device, const lsd::Mat4& view_proj, const lsd::editor::EditorScene& scene, lsd::usize index, bool selected) {
    const lsd::editor::SceneObject& o = scene.object(index);
    const lsd::render::GpuMesh& mesh = scene.mesh(o.mesh);
    lsd::rhi::MeshDraw draw{};
    draw.vertices = mesh.vertices;
    draw.indices = mesh.indices;
    draw.index_count = mesh.index_count;
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

// Copies a file dropped from Explorer into Content/<folder>/, renaming on clash ("Chair_1.fbx").
// The registry picks it up on the next scan and writes its .meta.
bool import_file(const char* source, const char* content_folder, char* status, lsd::usize capacity) {
    const fs::path src(source);
    if (lsd::asset::type_from_extension(src.extension().string().c_str()) == lsd::asset::AssetType::Unknown) {
        std::snprintf(status, capacity, "Cannot import %s (supported: .fbx, .gltf, .glb)", src.filename().string().c_str());
        return false;
    }
    std::error_code error;
    const fs::path folder = fs::path(kContentDir) / content_folder;
    fs::create_directories(folder, error);
    fs::path dest = folder / src.filename();
    for (int n = 1; fs::exists(dest, error); ++n) dest = folder / (src.stem().string() + "_" + std::to_string(n) + src.extension().string());
    if (!fs::copy_file(src, dest, error)) {
        std::snprintf(status, capacity, "Import failed: %s", error.message().c_str());
        return false;
    }
    // shortcut: a .gltf's external buffer is assumed to be <name>.bin next to it; parse the uri list when textures arrive.
    const fs::path bin = fs::path(src).replace_extension(".bin");
    if (src.extension() == ".gltf" && fs::exists(bin, error)) fs::copy_file(bin, folder / bin.filename(), error);
    std::snprintf(status, capacity, "Imported %s", dest.generic_string().c_str());
    LSD_LOG_INFO("editor", "%s", status);
    return true;
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
    if (fs::exists(kFont)) ImGui::GetIO().Fonts->AddFontFromFileTTF(kFont, 18.0f);
#endif
}

} // namespace

int main(int argc, char** argv) {
    // shortcut: startup mirrors runtime/main.cpp; fold both into a shared app layer when a third exe needs it.
    lsd::rhi::DeviceDesc desc{};
    long max_frames = -1;
    long screenshot_frame = -1;     // -screenshot=<n>: save frame n to Saved/screenshot.bmp (OpenGL)
    const char* import_arg = nullptr;  // -import=<file>: same as dropping the file on the editor
    const char* place_arg = nullptr;   // -place=<asset name>: place that asset at the origin on startup
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-gpu=", 5) == 0) desc.adapter_override = std::atoi(argv[i] + 5);
        else if (std::strcmp(argv[i], "-rhi=d3d12") == 0) desc.backend = lsd::rhi::Backend::D3D12;
        else if (std::strcmp(argv[i], "-rhi=vulkan") == 0) desc.backend = lsd::rhi::Backend::Vulkan;
        else if (std::strcmp(argv[i], "-rhi=opengl") == 0) desc.backend = lsd::rhi::Backend::OpenGL;
        else if (std::strcmp(argv[i], "-validation") == 0) desc.enable_validation = true;
        else if (std::strncmp(argv[i], "-frames=", 8) == 0) max_frames = std::atol(argv[i] + 8);
        else if (std::strncmp(argv[i], "-screenshot=", 12) == 0) screenshot_frame = std::atol(argv[i] + 12);
        else if (std::strncmp(argv[i], "-import=", 8) == 0) import_arg = argv[i] + 8;
        else if (std::strncmp(argv[i], "-place=", 7) == 0) place_arg = argv[i] + 7;
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
    fs::create_directories("Saved", ignored);
    fs::create_directories("Config", ignored);
    fs::create_directories(kContentDir, ignored);  // the project's assets, like Unreal's Content/

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

    lsd::asset::AssetRegistry registry(allocator, kContentDir);
    if (import_arg) import_file(import_arg, "", state.status, sizeof state.status);
    registry.scan();
    LSD_LOG_INFO("editor", "%zu assets in %s/", registry.count(), kContentDir);
    state.registry = &registry;

    // Owned explicitly: its GPU buffers must be released before the device is destroyed.
    auto* scene_memory = allocator.allocate(sizeof(lsd::editor::EditorScene), alignof(lsd::editor::EditorScene));
    lsd::editor::EditorScene& scene = *new (scene_memory) lsd::editor::EditorScene(*device, allocator, registry, kDataDir);
    auto* thumbnail_memory = allocator.allocate(sizeof(lsd::editor::ThumbnailCache), alignof(lsd::editor::ThumbnailCache));
    lsd::editor::ThumbnailCache& thumbnails = *new (thumbnail_memory) lsd::editor::ThumbnailCache(*device, allocator, registry, kDataDir);
    state.thumbnails = &thumbnails;
    {
        lsd::render::MeshData cube = lsd::render::make_cube(allocator);
        lsd::render::MeshData sphere = lsd::render::make_sphere(allocator);
        const lsd::f32 grey[4] = {0.7f, 0.7f, 0.72f, 1.0f};
        const lsd::f32 red[4] = {0.85f, 0.35f, 0.3f, 1.0f};
        scene.add_builtin("Cube", cube, {0.0f, 0.0f, 0.0f}, grey);
        scene.add_builtin("Sphere", sphere, {1.5f, 1.5f, 0.0f}, red);
    }
    if (place_arg) {
        lsd::usize hit = 0;
        if (registry.search(place_arg, lsd::asset::AssetType::Mesh, nullptr, &hit, 1) == 1) scene.place_asset(registry.get(hit).guid, {-2.0f, 0.0f, 0.0f});
        else LSD_LOG_WARN("editor", "-place: no mesh asset named like '%s'", place_arg);
    }
    lsd::Array<lsd::editor::OutlineRow> rows(allocator);

    lsd::render::GridColors current_grid = grid_colors(state.theme);
    lsd::render::GpuLines grid = lsd::render::create_grid(*device, allocator, current_grid);

    bool running = true;
    bool minimized = false;
    lsd::u64 last_scan = SDL_GetTicks();
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
                case SDL_EVENT_DROP_FILE:
                    if (import_file(event.drop.data, state.content_folder, state.status, sizeof state.status)) registry.scan();
                    break;
                default: break;
            }
        }
        if (minimized) { SDL_Delay(16); continue; }

        // Pick up new, changed and deleted files once a second; recompile what changed.
        if (SDL_GetTicks() - last_scan > 1000) {
            registry.scan();
            scene.hot_reload();
            last_scan = SDL_GetTicks();
        }

        // Outliner rows mirror the scene.
        rows.clear();
        for (lsd::usize i = 0; i < scene.count(); ++i) rows.push_back({scene.object(i).name, lsd::editor::RowKind::Mesh, 0});
        state.rows = rows.data();
        state.row_count = static_cast<int>(rows.size());

        device->debug_ui_new_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        lsd::editor::draw_editor(state);
        ImGui::Render();

        // Requests from the UI.
        if (state.place_requested) {
            state.place_requested = false;
            if (scene.place_asset(state.place_asset, state.place_position)) state.selected_row = static_cast<int>(scene.count() - 1);
            else std::snprintf(state.status, sizeof state.status, "Could not place the asset (see the log)");
        }
        if (state.delete_row >= 0) {
            scene.remove(static_cast<lsd::usize>(state.delete_row));
            state.delete_row = -1;
            state.selected_row = -1;
        }

        // Rebuild the grid when its theme colours change.
        const lsd::render::GridColors wanted_grid = grid_colors(state.theme);
        if (std::memcmp(&wanted_grid, &current_grid, sizeof wanted_grid) != 0) {
            lsd::render::destroy_lines(*device, grid);
            grid = lsd::render::create_grid(*device, allocator, wanted_grid);
            current_grid = wanted_grid;
        }

        const lsd::LinearColor& bg = state.theme.viewport_background;
        const lsd::f32 clear_color[4] = {bg.r, bg.g, bg.b, 1.0f};
        thumbnails.release_temporaries();
        device->begin_frame(clear_color);
        thumbnails.render(clear_color);
        const lsd::f32* rect = state.viewport_rect;
        if (rect[2] >= 8.0f && rect[3] >= 8.0f) {
            // ImGui works in window points; the swapchain is in pixels (differs on high-DPI screens).
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            device->set_viewport(rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y);
            const lsd::Mat4 view_proj = lsd::editor::camera_projection(state.camera, rect[2] / rect[3]) * lsd::editor::camera_view(state.camera);
            if (state.show_grid) device->draw_lines(grid.vertices, grid.vertex_count, view_proj.m);
            for (lsd::usize i = 0; i < scene.count(); ++i) draw_object(*device, view_proj, scene, i, state.selected_row == static_cast<int>(i));
        }
        device->render_debug_ui();
        if (frame == screenshot_frame && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl for now.");
        }
        device->end_frame();
        ++frame;
    }

    lsd::render::destroy_lines(*device, grid);
    thumbnails.~ThumbnailCache();
    allocator.deallocate(thumbnail_memory);
    scene.~EditorScene();
    allocator.deallocate(scene_memory);
    lsd::rhi::destroy_device(device, allocator);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
