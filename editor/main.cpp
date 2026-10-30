// LSD_Engine editor entry point: level editor (Viewport, Outliner, Details, Content Browser, Play).
#include "editor_ui.h"
#include "mesh_cache.h"
#include "play_session.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/system_info.h"
#include "lsd/render/grid.h"
#include "lsd/render/mesh.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/components.h"
#include "lsd/world/level.h"

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
using lsd::editor::EditorState;

constexpr char kContentDir[] = "Content";
constexpr char kDataDir[] = "DerivedData";
constexpr char kSettingsPath[] = "Saved/editor_settings.json";

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

// Every StaticMesh in the world, selection tinted.
void draw_world(lsd::rhi::Device& device, const lsd::Mat4& view_proj, lsd::world::World& world, lsd::editor::MeshCache& meshes,
                lsd::world::Entity selected) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
        const lsd::editor::CachedMesh* mesh = meshes.get(component->mesh);
        if (!mesh) continue;
        const lsd::world::Entity e = world.owner(type, i);
        const lsd::Mat4 m = world.world_matrix(e);
        const lsd::Mat4 mvp = view_proj * m;
        const lsd::Mat4 inv = lsd::affine_inverse(m);  // normals use the inverse transpose (correct under non-uniform scale)
        lsd::rhi::MeshDraw draw{};
        draw.vertices = mesh->gpu.vertices;
        draw.indices = mesh->gpu.indices;
        draw.index_count = mesh->gpu.index_count;
        std::memcpy(draw.mvp, mvp.m, sizeof draw.mvp);
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) draw.world_rotation[c * 4 + r] = inv.at(c, r);
            draw.world_rotation[c * 4 + 3] = 0.0f;
        }
        to_rgba(component->color, draw.color);
        if (e == selected) {  // shortcut: selection tint until the outline post-process exists
            draw.color[0] = 1.0f;
            draw.color[1] = 0.85f;
            draw.color[2] = 0.25f;
        }
        device.draw_mesh(draw);
    }
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

// ---- Level files ----

void new_level(EditorState& s) {
    lsd::editor::make_default_level(*s.world);
    s.level_path[0] = '\0';
    s.selected = {};
    s.history->reset(*s.world, "New Level");
    s.history->commit(*s.world, "Default objects");  // so the untitled level counts as unsaved
}

bool open_level(EditorState& s, const char* path) {
    if (!lsd::world::load_level_file(*s.world, path)) {
        std::snprintf(s.status, sizeof s.status, "Could not open %s", path);
        lsd::world::World& w = *s.world;
        if (w.slot_count() == 0) new_level(s);  // a failed load leaves the world empty
        return false;
    }
    std::snprintf(s.level_path, sizeof s.level_path, "%s", fs::path(path).generic_string().c_str());
    s.selected = {};
    s.history->reset(*s.world, "Open Level");
    std::snprintf(s.status, sizeof s.status, "Opened %s", s.level_path);
    LSD_LOG_INFO("editor", "%s", s.status);
    return true;
}

bool save_level(EditorState& s, const char* path) {
    std::error_code error;
    fs::create_directories(fs::path(path).parent_path(), error);
    if (!lsd::world::save_level_file(*s.world, path)) {
        std::snprintf(s.status, sizeof s.status, "Could not save %s", path);
        LSD_LOG_ERROR("editor", "%s", s.status);
        return false;
    }
    std::snprintf(s.level_path, sizeof s.level_path, "%s", fs::path(path).generic_string().c_str());
    s.history->mark_saved();
    s.registry->scan();  // a new level file appears in the Content Browser
    std::snprintf(s.status, sizeof s.status, "Saved %s", s.level_path);
    LSD_LOG_INFO("editor", "%s", s.status);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // shortcut: startup mirrors runtime/main.cpp; fold both into a shared app layer when a third exe needs it.
    lsd::rhi::DeviceDesc desc{};
    long max_frames = -1;
    long screenshot_frame = -1;        // -screenshot=<n>: save frame n to Saved/screenshot.bmp (OpenGL)
    const char* import_arg = nullptr;  // -import=<file>: same as dropping the file on the editor
    const char* place_arg = nullptr;   // -place=<asset name>: place that mesh asset at (-2, 0, 0) on startup
    const char* level_arg = nullptr;   // -level=<path>: open this level instead of the last one
    bool play_arg = false;             // -play: start Play-in-Editor right away
    const char* select_arg = nullptr;  // -select=<object name>: select it on startup (screenshots)
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
        else if (std::strncmp(argv[i], "-level=", 7) == 0) level_arg = argv[i] + 7;
        else if (std::strcmp(argv[i], "-play") == 0) play_arg = true;
        else if (std::strncmp(argv[i], "-select=", 8) == 0) select_arg = argv[i] + 8;
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

    // Placement-allocated: the state holds big buffers, and GPU owners must go before the device.
    auto* state_memory = allocator.allocate(sizeof(EditorState), alignof(EditorState));
    EditorState& state = *new (state_memory) EditorState();
    lsd::editor::load_theme(state.theme);
    lsd::editor::apply_theme(state.theme);
    lsd::reflect::load_file(state.settings, kSettingsPath);

    lsd::asset::AssetRegistry registry(allocator, kContentDir);
    if (import_arg) import_file(import_arg, "", state.status, sizeof state.status);
    registry.scan();
    LSD_LOG_INFO("editor", "%zu assets in %s/", registry.count(), kContentDir);
    state.registry = &registry;

    lsd::world::World world(allocator);
    lsd::editor::Transactions history(allocator);
    auto* mesh_memory = allocator.allocate(sizeof(lsd::editor::MeshCache), alignof(lsd::editor::MeshCache));
    lsd::editor::MeshCache& meshes = *new (mesh_memory) lsd::editor::MeshCache(*device, allocator, registry, kDataDir);
    auto* thumbnail_memory = allocator.allocate(sizeof(lsd::editor::ThumbnailCache), alignof(lsd::editor::ThumbnailCache));
    lsd::editor::ThumbnailCache& thumbnails = *new (thumbnail_memory) lsd::editor::ThumbnailCache(*device, allocator, registry, kDataDir);
    state.world = &world;
    state.history = &history;
    state.meshes = &meshes;
    state.thumbnails = &thumbnails;

    const char* startup_level = level_arg ? level_arg : state.settings.last_level;
    if (!startup_level[0] || !fs::exists(startup_level) || !open_level(state, startup_level)) new_level(state);
    if (place_arg) {
        lsd::usize hit = 0;
        if (registry.search(place_arg, lsd::asset::AssetType::Mesh, nullptr, &hit, 1) == 1) {
            const lsd::asset::AssetRecord& r = registry.get(hit);
            char name[64];
            world.unique_name(r.name, name, sizeof name);
            const lsd::world::Entity e = world.create(name);
            world.get<lsd::world::Transform>(e)->position = {-2.0f, 0.0f, 0.0f};
            lsd::asset::to_string(r.guid, world.add<lsd::world::StaticMesh>(e)->mesh);
            history.commit(world, "Place");
        } else {
            LSD_LOG_WARN("editor", "-place: no mesh asset named like '%s'", place_arg);
        }
    }
    state.play_requested = play_arg;
    for (lsd::u32 slot = 0; select_arg && slot < world.slot_count(); ++slot) {
        const lsd::world::Entity e = world.entity_at_slot(slot);
        if (e.valid() && std::strcmp(world.name(e), select_arg) == 0) state.selected = e;
    }

    lsd::render::GridColors current_grid = grid_colors(state.theme);
    lsd::render::GpuLines grid = lsd::render::create_grid(*device, allocator, current_grid);

    lsd::editor::PlaySession play;
    lsd::Array<char> play_snapshot(allocator);  // the level before Play, restored on Stop
    lsd::f32 mouse_dx = 0.0f, mouse_dy = 0.0f;
    char title[400] = "";

    bool running = true;
    bool minimized = false;
    lsd::u64 last_scan = SDL_GetTicks();
    lsd::u64 last_tick = SDL_GetTicksNS();
    for (long frame = 0; running && frame != max_frames;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    if (history.dirty() && !state.playing && max_frames < 0) state.quit_requested = true;
                    else running = false;
                    break;
                case SDL_EVENT_WINDOW_MINIMIZED: minimized = true; break;
                case SDL_EVENT_WINDOW_RESTORED: minimized = false; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: state.mouse_captured = false; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    device->resize_swapchain(static_cast<lsd::u32>(event.window.data1), static_cast<lsd::u32>(event.window.data2));
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    mouse_dx += event.motion.xrel;
                    mouse_dy += event.motion.yrel;
                    break;
                case SDL_EVENT_DROP_FILE:
                    if (import_file(event.drop.data, state.content_folder, state.status, sizeof state.status)) registry.scan();
                    break;
                default: break;
            }
        }
        if (minimized) { SDL_Delay(16); continue; }
        const lsd::u64 now = SDL_GetTicksNS();
        const lsd::f32 dt = static_cast<lsd::f32>(static_cast<double>(now - last_tick) * 1e-9);
        last_tick = now;

        // Pick up new, changed and deleted files once a second; recompile what changed.
        if (SDL_GetTicks() - last_scan > 1000) {
            registry.scan();
            meshes.hot_reload();
            last_scan = SDL_GetTicks();
        }

        device->debug_ui_new_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        lsd::editor::draw_editor(state);
        ImGui::Render();

        // ---- Requests from the UI ----
        if (state.new_level_requested) {
            state.new_level_requested = false;
            new_level(state);
        }
        if (state.open_level_requested[0]) {
            open_level(state, state.open_level_requested);
            state.open_level_requested[0] = '\0';
        }
        bool saved = false;
        if (state.save_requested) {
            state.save_requested = false;
            saved = save_level(state, state.level_path);
        }
        if (state.save_as_requested[0]) {
            saved = save_level(state, state.save_as_requested);
            state.save_as_requested[0] = '\0';
        }
        if (state.quit_confirmed || (state.quit_after_save && saved)) running = false;

        if (state.play_requested && !state.playing) {
            lsd::world::save_level(world, play_snapshot);
            play.start(world, state.camera.position, state.camera.yaw, state.camera.pitch);
            state.playing = true;
            state.mouse_captured = true;
            state.selected = {};
            LSD_LOG_INFO("editor", "Play");
        }
        state.play_requested = false;
        if (state.stop_requested && state.playing) {
            lsd::world::load_level(world, play_snapshot.data(), play_snapshot.size());  // Play changes revert, like Unreal's PIE
            state.playing = false;
            state.mouse_captured = false;
            LSD_LOG_INFO("editor", "Stop");
        }
        state.stop_requested = false;
        SDL_SetWindowRelativeMouseMode(window, state.playing && state.mouse_captured);

        if (state.playing) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            lsd::editor::PlayInput input{};
            if (state.mouse_captured) {
                input = {keys[SDL_SCANCODE_W], keys[SDL_SCANCODE_S], keys[SDL_SCANCODE_A], keys[SDL_SCANCODE_D], keys[SDL_SCANCODE_SPACE],
                         keys[SDL_SCANCODE_LSHIFT], mouse_dx, mouse_dy};
            }
            play.update(world, meshes, input, dt);
        }
        mouse_dx = mouse_dy = 0.0f;

        // Window title: level name and unsaved marker, like Unreal.
        char wanted_title[400];
        std::snprintf(wanted_title, sizeof wanted_title, "%s%s - LSD_Engine Editor%s", state.level_path[0] ? fs::path(state.level_path).stem().string().c_str() : "Untitled",
                      history.dirty() ? "*" : "", state.playing ? " (Playing)" : "");
        if (std::strcmp(wanted_title, title) != 0) {
            std::snprintf(title, sizeof title, "%s", wanted_title);
            SDL_SetWindowTitle(window, title);
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
            const lsd::Mat4 proj = lsd::editor::camera_projection(state.camera, rect[2] / rect[3]);
            const lsd::Mat4 view = state.playing ? lsd::look_at(play.eye(), play.eye() + play.forward(), lsd::Vec3{0, 0, 1}) : lsd::editor::camera_view(state.camera);
            const lsd::Mat4 view_proj = proj * view;
            if (state.show_grid && !state.game_view && !state.playing) device->draw_lines(grid.vertices, grid.vertex_count, view_proj.m);
            draw_world(*device, view_proj, world, meshes, state.playing ? lsd::world::Entity{} : state.selected);
        }
        device->render_debug_ui();
        if (frame == screenshot_frame && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl for now.");
        }
        device->end_frame();
        ++frame;
    }

    std::snprintf(state.settings.last_level, sizeof state.settings.last_level, "%s", state.level_path);
    lsd::reflect::save_file(state.settings, kSettingsPath);
    SDL_SetWindowRelativeMouseMode(window, false);
    lsd::render::destroy_lines(*device, grid);
    thumbnails.~ThumbnailCache();
    allocator.deallocate(thumbnail_memory);
    meshes.~MeshCache();
    allocator.deallocate(mesh_memory);
    state.~EditorState();
    allocator.deallocate(state_memory);
    lsd::rhi::destroy_device(device, allocator);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
