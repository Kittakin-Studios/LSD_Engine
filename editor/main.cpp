// LSD_Engine editor entry point: level editor (Viewport, Outliner, Details, Content Browser, Play).
#include "editor_ui.h"
#include "graph_editor.h"
#include "table_editor.h"
#include "mesh_cache.h"
#include "lighting.h"
#include "material_cache.h"
#include "animation_player.h"
#include "particle_player.h"
#include "skinned_cache.h"
#include "play_session.h"
#include "sound_player.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/asset/data_asset.h"
#include "lsd/core/console.h"
#include "lsd/input/input.h"
#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/world/curve.h"
#include "lsd/render/material.h"
#include "lsd/audio/audio.h"
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
#include <string>

namespace {

namespace fs = std::filesystem;
using lsd::editor::EditorState;

constexpr char kContentDir[] = "Content";
constexpr char kDataDir[] = "DerivedData";
constexpr char kSettingsPath[] = "Saved/editor_settings.json";
constexpr char kLockPath[] = "Saved/editor.lock";  // exists while the editor runs; left behind by a crash
constexpr char kAutosaveDir[] = "Saved/Autosaves";

// Saved/Autosaves/<level name>.lsdlevel ("Untitled" before the first save).
std::string autosave_path(const char* level_path) {
    return std::string(kAutosaveDir) + "/" + (level_path[0] ? fs::path(level_path).stem().string() : std::string("Untitled")) + lsd::world::kLevelExtension;
}

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

// Every StaticMesh in the world, selection tinted (not while playing). Counts draws and triangles for `stat fps`.
// Skinned meshes for drawing and shadows: their model and current pose (set up by main()).
lsd::editor::SkinnedCache* g_skinned = nullptr;
lsd::editor::AnimationPlayer* g_animations = nullptr;

// Skinned shadow casters (within `range` of `light` when range > 0).
void draw_skinned_casters(lsd::rhi::Device& device, const lsd::Mat4& vp, lsd::world::World& world, bool skip_hidden, const lsd::Vec3* light, lsd::f32 range) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::SkeletalMesh>();
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const lsd::world::Entity e = world.owner(type, i);
        if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
        const lsd::editor::SkinnedEntry* model = g_skinned->get(static_cast<const lsd::world::SkeletalMesh*>(world.at(type, i))->mesh);
        const lsd::anim::Pose* pose = g_animations->pose(e);
        if (!model || !pose || !pose->joint_count()) continue;
        const lsd::Mat4 m = world.world_matrix(e);
        if (light && range > 0.0f) {
            const lsd::Vec3 centre = lsd::transform_point(m, (model->model->bounds_min + model->model->bounds_max) * 0.5f);
            const lsd::Vec3 extent = lsd::transform_vector(m, (model->model->bounds_max - model->model->bounds_min) * 0.5f);
            if (lsd::length(centre - *light) > range + lsd::length(extent) * 2.0f) continue;  // generous: the pose may reach out
        }
        const lsd::Mat4 mvp = vp * m;
        device.draw_shadow_caster(model->vertices, model->indices, model->index_count, mvp.m, pose->joint_rows(), pose->joint_count());
    }
}

// The sun's shadow cascades: every visible mesh into each tile of the atlas, then back to the window.
void draw_shadows(lsd::rhi::Device& device, const lsd::rhi::LightSetup& lights, lsd::rhi::TextureHandle atlas, lsd::world::World& world,
                  lsd::editor::MeshCache& meshes, bool skip_hidden) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    device.begin_shadow_pass(atlas);
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::kShadowAtlasSize);
    for (lsd::u32 c = 0; c < static_cast<lsd::u32>(lights.shadow_info[0]); ++c) {
        device.set_viewport(lights.shadow_tiles[c][0] * size, lights.shadow_tiles[c][1] * size, lights.shadow_tiles[c][2] * size, lights.shadow_tiles[c][2] * size);
        lsd::Mat4 vp;
        std::memcpy(vp.m, lights.shadow_matrices[c], sizeof vp.m);
        for (lsd::u32 i = 0; i < world.count(type); ++i) {
            const lsd::world::Entity e = world.owner(type, i);
            if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
            const lsd::editor::CachedMesh* mesh = meshes.get(static_cast<const lsd::world::StaticMesh*>(world.at(type, i))->mesh);
            if (!mesh) continue;
            const lsd::Mat4 mvp = vp * world.world_matrix(e);
            device.draw_shadow_caster(mesh->gpu.vertices, mesh->gpu.indices, mesh->gpu.index_count, mvp.m);
        }
        draw_skinned_casters(device, vp, world, skip_hidden, nullptr, 0.0f);
    }
    device.end_shadow_pass();
}

// Spot / point light shadow tiles: the meshes that reach into each light's range.
void draw_local_shadows(lsd::rhi::Device& device, const lsd::rhi::LightSetup& lights, lsd::rhi::TextureHandle atlas, lsd::world::World& world,
                        lsd::editor::MeshCache& meshes, bool skip_hidden) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    device.begin_shadow_pass(atlas);
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::kShadowAtlasSize);
    for (lsd::u32 li = 0; li < lights.count; ++li) {
        const lsd::rhi::GpuLight& light = lights.lights[li];
        if (light.shadow_tile < 0.0f) continue;
        const lsd::u32 first = static_cast<lsd::u32>(light.shadow_tile);
        const lsd::u32 tiles = light.type == static_cast<lsd::f32>(lsd::rhi::LightType::Point) ? 6u : 1u;
        const lsd::Vec3 at{light.position[0], light.position[1], light.position[2]};
        for (lsd::u32 t = first; t < first + tiles; ++t) {
            device.set_viewport(lights.local_tiles[t][0] * size, lights.local_tiles[t][1] * size, lights.local_tiles[t][2] * size, lights.local_tiles[t][2] * size);
            lsd::Mat4 vp;
            std::memcpy(vp.m, lights.local_matrices[t], sizeof vp.m);
            for (lsd::u32 i = 0; i < world.count(type); ++i) {
                const lsd::world::Entity e = world.owner(type, i);
                if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
                const lsd::editor::CachedMesh* mesh = meshes.get(static_cast<const lsd::world::StaticMesh*>(world.at(type, i))->mesh);
                if (!mesh) continue;
                const lsd::Mat4 m = world.world_matrix(e);
                // Skip meshes whose bounding sphere does not reach the light.
                const lsd::Vec3 centre = lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f);
                const lsd::Vec3 extent = lsd::transform_vector(m, (mesh->bounds_max - mesh->bounds_min) * 0.5f);
                if (lsd::length(centre - at) > light.range + lsd::length(extent) * 1.8f) continue;
                const lsd::Mat4 mvp = vp * m;
                device.draw_shadow_caster(mesh->gpu.vertices, mesh->gpu.indices, mesh->gpu.index_count, mvp.m);
            }
            draw_skinned_casters(device, vp, world, skip_hidden, &at, light.range);
        }
    }
    device.end_shadow_pass();
}

void draw_world(lsd::rhi::Device& device, const lsd::Mat4& view_proj, const lsd::editor::LightView& light_view, const lsd::f32 viewport[4],
                lsd::rhi::TextureHandle shadow_atlas, lsd::rhi::TextureHandle local_atlas, lsd::world::World& world, lsd::editor::MeshCache& meshes, lsd::editor::MaterialCache& materials,
                EditorState& stats) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    const lsd::Vec3 eye = light_view.eye;
    const bool skip_hidden = !stats.playing || stats.ejected;
    const lsd::rhi::LightSetup lights = lsd::editor::world_lights(world, light_view, skip_hidden);
    const bool sun_shadows = lights.shadow_info[1] >= 0.0f && shadow_atlas.valid() && !stats.view_mode_unlit_or_wire();
    const bool local_shadows = lights.local_tiles[0][3] > 0.0f && local_atlas.valid() && !stats.view_mode_unlit_or_wire();
    if (sun_shadows) draw_shadows(device, lights, shadow_atlas, world, meshes, skip_hidden);
    if (local_shadows) draw_local_shadows(device, lights, local_atlas, world, meshes, skip_hidden);
    if (sun_shadows || local_shadows) device.set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    device.set_shadow_map(sun_shadows ? shadow_atlas : lsd::rhi::TextureHandle{}, local_shadows ? local_atlas : lsd::rhi::TextureHandle{});
    device.set_lights(lights);
    stats.stat_draws = 0;
    stats.stat_triangles = 0;
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
        const lsd::world::Entity e = world.owner(type, i);
        if ((!stats.playing || stats.ejected) && world.hidden_in_hierarchy(e)) continue;  // hidden in the editor, not in the game
        const lsd::editor::CachedMesh* mesh = meshes.get(component->mesh);
        if (!mesh) continue;
        const lsd::Mat4 m = world.world_matrix(e);
        const lsd::Mat4 mvp = view_proj * m;
        const lsd::Mat4 inv = lsd::affine_inverse(m);  // normals use the inverse transpose (correct under non-uniform scale)
        lsd::rhi::MeshDraw draw{};
        draw.vertices = mesh->gpu.vertices;
        draw.indices = mesh->gpu.indices;
        draw.index_count = mesh->gpu.index_count;
        std::memcpy(draw.mvp, mvp.m, sizeof draw.mvp);
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) draw.normal_matrix[c * 4 + r] = inv.at(c, r);
            draw.normal_matrix[c * 4 + 3] = 0.0f;
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) draw.world[r * 4 + c] = m.at(r, c);
        }
        draw.camera_position[0] = eye.x;
        draw.camera_position[1] = eye.y;
        draw.camera_position[2] = eye.z;
        to_rgba(component->color, draw.color);
        materials.apply(component->material, draw);  // base colour x tint, textures, metallic / roughness, emissive
        const bool editor_view = !stats.playing || stats.ejected;
        draw.unlit = editor_view && stats.view_mode == lsd::editor::ViewMode::Unlit;
        draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
        if ((!stats.playing || stats.ejected) && stats.is_selected(e)) {  // shortcut: selection tint until the outline post-process exists
            draw.color[0] = 1.0f;
            draw.color[1] = 0.85f;
            draw.color[2] = 0.25f;
        }
        device.draw_mesh(draw);
        ++stats.stat_draws;
        stats.stat_triangles += mesh->gpu.index_count / 3;
    }
    // Skeletal meshes: same material path, skinned on the GPU with their current pose.
    const lsd::reflect::TypeInfo& skinned_type = lsd::reflect::type_of<lsd::world::SkeletalMesh>();
    for (lsd::u32 i = 0; i < world.count(skinned_type); ++i) {
        const auto* component = static_cast<const lsd::world::SkeletalMesh*>(world.at(skinned_type, i));
        const lsd::world::Entity e = world.owner(skinned_type, i);
        if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
        const lsd::editor::SkinnedEntry* model = g_skinned->get(component->mesh);
        const lsd::anim::Pose* pose = g_animations->pose(e);
        if (!model || !pose || !pose->joint_count()) continue;
        const lsd::Mat4 m = world.world_matrix(e);
        const lsd::Mat4 mvp = view_proj * m;
        const lsd::Mat4 inv = lsd::affine_inverse(m);
        lsd::rhi::MeshDraw draw{};
        draw.vertices = model->vertices;
        draw.indices = model->indices;
        draw.index_count = model->index_count;
        draw.joint_rows = pose->joint_rows();
        draw.joint_count = pose->joint_count();
        std::memcpy(draw.mvp, mvp.m, sizeof draw.mvp);
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) draw.normal_matrix[c * 4 + r] = inv.at(c, r);
            draw.normal_matrix[c * 4 + 3] = 0.0f;
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) draw.world[r * 4 + c] = m.at(r, c);
        }
        draw.camera_position[0] = eye.x;
        draw.camera_position[1] = eye.y;
        draw.camera_position[2] = eye.z;
        to_rgba(component->color, draw.color);
        materials.apply(component->material, draw);
        const bool editor_view = !stats.playing || stats.ejected;
        draw.unlit = editor_view && stats.view_mode == lsd::editor::ViewMode::Unlit;
        draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
        if (editor_view && stats.is_selected(e)) {
            draw.color[0] = 1.0f;
            draw.color[1] = 0.85f;
            draw.color[2] = 0.25f;
        }
        device.draw_mesh(draw);
        ++stats.stat_draws;
        stats.stat_triangles += model->index_count / 3;
    }
}

// Console commands and variables (Output Log tab), like Unreal's exec commands and CVars.
void register_console(EditorState& s) {
    namespace console = lsd::console;
    console::add_variable("r.Grid", &s.show_grid, "Draw the floor grid");
    console::add_variable("editor.GameView", &s.game_view, "Hide editor helpers (G)");
    console::add_variable("editor.CameraSpeed", &s.settings.camera_speed, "Viewport fly speed in m/s");
    console::add_variable("editor.Snap", &s.settings.snap, "Snap gizmo moves, rotations and scales");
    console::add_variable("editor.OrthoHeight", &s.camera.ortho_height, "Metres visible top to bottom in the orthographic views");
    console::add_variable("editor.SnapMove", &s.settings.snap_move, "Move snap step in metres");
    console::add_variable("editor.SnapAngle", &s.settings.snap_angle, "Rotate snap step in degrees");
    console::add_command("help", [](void*, const char* args) {
        const char* names[128];
        const lsd::u32 n = console::complete(args, names, 128);
        for (lsd::u32 i = 0; i < n; ++i) LSD_LOG_INFO("console", "  %-22s %s", names[i], console::help(names[i]));
    }, &s, "List commands and variables (help <prefix> filters)");
    console::add_command("clear", [](void*, const char*) { lsd::editor::clear_output_log(); }, &s, "Clear the Output Log");
    console::add_command("stat", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        if (std::strcmp(args, "fps") == 0 || std::strcmp(args, "unit") == 0) st.show_stats = !st.show_stats;
        else if (std::strcmp(args, "audio") == 0) st.show_audio_stats = !st.show_audio_stats;
        else LSD_LOG_WARN("console", "stat: try 'stat fps' or 'stat audio'");
    }, &s, "stat fps | stat audio: toggle the fps / draw call or the active sounds overlay");
    console::add_command("view", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        static const char* const names[] = {"perspective", "top", "front", "right"};
        for (int v = 0; v < 4; ++v) {
            if (std::strcmp(args, names[v]) == 0) return lsd::editor::set_view(st, static_cast<lsd::editor::ViewType>(v));
        }
        LSD_LOG_WARN("console", "view: perspective, top, front or right");
    }, &s, "view perspective|top|front|right");
    console::add_command("layout", [](void* user, const char* args) {
        lsd::editor::set_layout(*static_cast<EditorState*>(user), std::strcmp(args, "4") == 0 || std::strcmp(args, "2x2") == 0);
    }, &s, "layout 1|4: one viewport or perspective + top + front + right");
    console::add_command("input.CreateDefaults", [](void* user, const char*) { lsd::editor::create_input_setup(*static_cast<EditorState*>(user)); },
                         &s, "Create IA_Move, IA_Look, IA_Jump and IMC_Default in the open folder");
    console::add_command("asset.Select", [](void* user, const char* args) {
        if (!lsd::editor::select_asset(*static_cast<EditorState*>(user), args)) LSD_LOG_WARN("console", "asset.Select: no asset named like '%s'", args);
    }, &s, "asset.Select <name>: select an asset and show it in Details");
    console::add_command("graph.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Graph, nullptr, &hit, 1) == 1 || st.registry->search(args, lsd::asset::AssetType::Cue, nullptr, &hit, 1) == 1) {
            lsd::editor::open_graph(st, st.registry->get(hit));
        } else {
            LSD_LOG_WARN("editor", "graph.Open: no visual script or sound cue named like '%s'", args);
        }
    }, &s, "Open a visual script or sound cue in the graph editor");
    console::add_command("table.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Table, nullptr, &hit, 1) == 1) lsd::editor::open_table(st, st.registry->get(hit));
        else LSD_LOG_WARN("editor", "table.Open: no data table named like '%s'", args);
    }, &s, "Open a data table in the table editor");
    console::add_command("viewmode", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        static const char* const names[] = {"lit", "unlit", "wireframe"};
        for (int m = 0; m < 3; ++m) {
            if (std::strcmp(args, names[m]) == 0) {
                st.view_mode = static_cast<lsd::editor::ViewMode>(m);
                return;
            }
        }
        LSD_LOG_WARN("console", "viewmode: lit, unlit or wireframe");
    }, &s, "viewmode lit|unlit|wireframe");
    console::add_command("play", [](void* user, const char*) { static_cast<EditorState*>(user)->play_requested = true; }, &s, "Start Play-in-Editor");
    console::add_command("stop", [](void* user, const char*) { static_cast<EditorState*>(user)->stop_requested = true; }, &s, "Stop Play-in-Editor");
    console::add_command("save", [](void* user, const char*) {
        auto& st = *static_cast<EditorState*>(user);
        if (st.level_path[0]) st.save_requested = true;
        else LSD_LOG_WARN("console", "save: the level has no file yet; use File > Save Level As");
    }, &s, "Save the current level");
    console::add_command("open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Level, nullptr, &hit, 1) == 1) st.registry->source_path(st.registry->get(hit), st.open_level_requested, sizeof st.open_level_requested);
        else LSD_LOG_WARN("console", "open: no level named like '%s'", args);
    }, &s, "open <level name>: open a level (discards unsaved changes)");
}

// Copies a file dropped from Explorer into Content/<folder>/, renaming on clash ("Chair_1.fbx").
// The registry picks it up on the next scan and writes its .meta.
bool import_file(const char* source, const char* content_folder, char* status, lsd::usize capacity) {
    const fs::path src(source);
    if (lsd::asset::type_from_extension(src.extension().string().c_str()) == lsd::asset::AssetType::Unknown) {
        std::snprintf(status, capacity, "Cannot import %s (supported: .fbx, .gltf, .glb, .csv, .wav, .ogg, .flac, .mp3)", src.filename().string().c_str());
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
    lsd::editor::refresh_prefabs(s);  // prefabs edited since the level was saved show their latest version
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
    lsd::editor::install_output_log();
    // shortcut: startup mirrors runtime/main.cpp; fold both into a shared app layer when a third exe needs it.
    lsd::rhi::DeviceDesc desc{};
    long max_frames = -1;
    long screenshot_frame = -1;        // -screenshot=<n>: save frame n to Saved/screenshot.bmp (OpenGL)
    const char* import_arg = nullptr;  // -import=<file>: same as dropping the file on the editor
    const char* place_arg = nullptr;   // -place=<asset name>: place that mesh asset at (-2, 0, 0) on startup
    const char* level_arg = nullptr;   // -level=<path>: open this level instead of the last one
    bool no_sound = false;             // -nosound: no audio device (automated runs stay quiet)
    bool play_arg = false;             // -play: start Play-in-Editor right away
    const char* select_arg = nullptr;  // -select=<object name>: select it on startup (screenshots)
    const char* exec_args[8] = {};     // -exec="<console line>": run after startup (repeatable)
    int exec_count = 0;
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
        else if (std::strcmp(argv[i], "-nosound") == 0) no_sound = true;
        else if (std::strcmp(argv[i], "-play") == 0) play_arg = true;
        else if (std::strncmp(argv[i], "-select=", 8) == 0) select_arg = argv[i] + 8;
        else if (std::strncmp(argv[i], "-exec=", 6) == 0 && exec_count < 8) exec_args[exec_count++] = argv[i] + 6;
        else LSD_LOG_WARN("editor", "Unknown argument: %s", argv[i]);
    }

    LSD_LOG_INFO("editor", "LSD_Engine Editor 0.1.0");
    const lsd::SystemInfo info = lsd::query_system_info();
    lsd::Allocator& allocator = lsd::default_allocator();
    lsd::JobSystem jobs(allocator, lsd::recommended_worker_count(info));

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
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
    lsd::reflect::load_file(state.project, "Config/project.json");

    // Types that can be created as data assets (Content Browser > New Data Asset).
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::input::InputAction>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::input::InputMappingContext>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::GameMode>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::Curve>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::render::Material>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::render::MaterialInstance>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundClass>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundAttenuation>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundConcurrency>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundMix>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::Submix>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::anim::AnimGraph>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::particles::ParticleSystem>());

    lsd::asset::AssetRegistry registry(allocator, kContentDir);
    if (import_arg) import_file(import_arg, "", state.status, sizeof state.status);
    registry.scan();
    LSD_LOG_INFO("editor", "%zu assets in %s/", registry.count(), kContentDir);
    state.registry = &registry;

    lsd::world::World world(allocator);
    lsd::editor::Transactions history(allocator);
    auto* mesh_memory = allocator.allocate(sizeof(lsd::editor::MeshCache), alignof(lsd::editor::MeshCache));
    lsd::editor::MeshCache& meshes = *new (mesh_memory) lsd::editor::MeshCache(*device, allocator, registry, kDataDir);
    auto* texture_cache = new lsd::editor::TextureCache(*device, registry);  // deleted before the device
    const lsd::rhi::TextureHandle shadow_atlas = device->create_shadow_map(lsd::editor::kShadowAtlasSize);
    const lsd::rhi::TextureHandle local_shadow_atlas = device->create_shadow_map(lsd::editor::kShadowAtlasSize);
    lsd::editor::MaterialCache materials(*texture_cache, registry);
    auto* skinned_cache = new lsd::editor::SkinnedCache(*device, registry);  // deleted before the device
    auto* animations = new lsd::editor::AnimationPlayer();
    auto* effects = new lsd::editor::ParticlePlayer(*device, allocator, registry, *texture_cache);  // deleted before the textures
    g_skinned = skinned_cache;
    g_animations = animations;
    auto* thumbnail_memory = allocator.allocate(sizeof(lsd::editor::ThumbnailCache), alignof(lsd::editor::ThumbnailCache));
    lsd::editor::ThumbnailCache& thumbnails = *new (thumbnail_memory) lsd::editor::ThumbnailCache(*device, allocator, registry, kDataDir);
    state.world = &world;
    state.history = &history;
    state.meshes = &meshes;
    state.thumbnails = &thumbnails;
    state.textures = texture_cache;
    state.skinned = skinned_cache;
    state.device = device;

    const char* startup_level = level_arg ? level_arg : state.settings.last_level[0] ? state.settings.last_level : state.project.editor_startup_level;
    if (!startup_level[0] || !fs::exists(startup_level) || !open_level(state, startup_level)) new_level(state);

    // Crash recovery: a lock file left behind means the last session did not exit cleanly.
    if (fs::exists(kLockPath)) {
        const std::string autosave = autosave_path(state.level_path);
        std::error_code error;
        const bool newer = fs::exists(autosave) && (!state.level_path[0] || fs::last_write_time(autosave, error) > fs::last_write_time(state.level_path, error));
        if (newer && max_frames < 0) std::snprintf(state.recovery_path, sizeof state.recovery_path, "%s", autosave.c_str());
    }
    if (FILE* lock = std::fopen(kLockPath, "wb")) std::fclose(lock);
    lsd::u64 last_autosave = SDL_GetTicks();
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
    register_console(state);
    for (int i = 0; i < exec_count; ++i) lsd::console::execute(exec_args[i]);
    for (lsd::u32 slot = 0; select_arg && slot < world.slot_count(); ++slot) {
        const lsd::world::Entity e = world.entity_at_slot(slot);
        if (e.valid() && std::strcmp(world.name(e), select_arg) == 0) {
            state.selected = e;
            state.selection[state.selection_count++] = e;
        }
    }

    lsd::render::GridColors current_grid = grid_colors(state.theme);
    lsd::render::GpuLines grid = lsd::render::create_grid(*device, allocator, current_grid);

    lsd::audio::AudioSystem audio;
    audio.init(!no_sound);
    lsd::editor::SoundPlayer sounds;
    sounds.bind(&audio, &registry);
    state.sounds = &sounds;
    lsd::editor::PlaySession play;
    play.set_sounds(&sounds);
    play.set_animations(animations);
    lsd::console::add_command("lua", [](void* user, const char* args) {
        if (!static_cast<lsd::editor::PlaySession*>(user)->run_lua(args)) LSD_LOG_WARN("script", "lua: runs while playing (see the Output Log for errors)");
    }, &play, "Run Lua while playing, e.g. lua lsd.log(lsd.time())");
    lsd::Array<char> play_snapshot(allocator);  // the level before Play, restored on Stop
    lsd::f32 mouse_dx = 0.0f, mouse_dy = 0.0f;
    SDL_Gamepad* gamepad = nullptr;  // the first one plugged in drives the player
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
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) {
                        gamepad = SDL_OpenGamepad(event.gdevice.which);
                        if (gamepad) LSD_LOG_INFO("editor", "Gamepad: %s", SDL_GetGamepadName(gamepad));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
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
            texture_cache->check_files();
            skinned_cache->check_files();
            effects->check_files();
            materials.check_files();
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
        if (state.restore_requested) {
            state.restore_requested = false;
            if (lsd::world::load_level_file(world, state.recovery_path)) {
                history.reset(world, "Restore Autosave");
                history.commit(world, "Restored autosave");  // unsaved until the user saves
                std::snprintf(state.status, sizeof state.status, "Restored %s", state.recovery_path);
            }
            state.recovery_path[0] = '\0';
        }
        const lsd::f32 autosave_ms = state.settings.autosave_minutes * 60000.0f;
        if (autosave_ms > 0.0f && SDL_GetTicks() - last_autosave > static_cast<lsd::u64>(autosave_ms)) {
            last_autosave = SDL_GetTicks();
            if (history.dirty() && !state.playing) {
                fs::create_directories(kAutosaveDir, ignored);
                const std::string path = autosave_path(state.level_path);
                if (lsd::world::save_level_file(world, path.c_str())) LSD_LOG_INFO("editor", "Autosaved to %s", path.c_str());
            }
        }

        if (state.play_requested && !state.playing) {
            lsd::world::save_level(world, play_snapshot);
            play.start(world, meshes, registry, state.camera.position, state.camera.yaw, state.camera.pitch);
            animations->reset();  // Play starts every animation from its beginning
            effects->reset();
            state.playing = true;
            state.mouse_captured = true;
            state.paused = state.ejected = false;
            LSD_LOG_INFO("editor", "Play");
        }
        state.play_requested = false;
        if (state.stop_requested && state.playing) {
            lsd::world::load_level(world, play_snapshot.data(), play_snapshot.size());  // Play changes revert, like Unreal's PIE
            state.playing = false;
            state.mouse_captured = false;
            state.paused = state.ejected = false;
            play.stop();
            LSD_LOG_INFO("editor", "Stop");
        }
        state.stop_requested = false;
        SDL_SetWindowRelativeMouseMode(window, state.playing && state.mouse_captured && !state.ejected);

        if (state.playing) {
            // Devices go to the game only while it has the mouse (not when ejected or the mouse is freed).
            lsd::input::InputFrame devices;
            if (state.mouse_captured && !state.ejected) {
                devices.keys = SDL_GetKeyboardState(&devices.key_count);
                devices.mouse_dx = mouse_dx;
                devices.mouse_dy = mouse_dy;
                const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                devices.mouse_buttons = ((buttons & SDL_BUTTON_LMASK) ? 1u : 0u) | ((buttons & SDL_BUTTON_MMASK) ? 2u : 0u) | ((buttons & SDL_BUTTON_RMASK) ? 4u : 0u);
                if (gamepad) {
                    const SDL_GamepadAxis axes[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                                     SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
                    for (int a = 0; a < 6; ++a) devices.gamepad_axes[a] = static_cast<lsd::f32>(SDL_GetGamepadAxis(gamepad, axes[a])) / 32767.0f;
                    devices.gamepad_axes[1] = -devices.gamepad_axes[1];  // SDL's stick Y points down; ours points up
                    devices.gamepad_axes[3] = -devices.gamepad_axes[3];
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b) {
                        if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(b))) devices.gamepad_buttons |= 1u << b;
                    }
                }
            }
            if (!state.paused || state.step_requested) play.update(world, devices, state.paused ? 1.0f / 60.0f : dt);
            state.step_requested = false;
            state.player_feet = play.feet();
        }
        // Skeletal meshes animate in the editor too; notifies (sounds) only in Play.
        animations->update(world, *skinned_cache, state.playing && state.paused && !state.step_requested ? 0.0f : dt,
                           state.playing && !state.paused ? &sounds : nullptr);
        effects->update(world, state.playing && state.paused && !state.step_requested ? 0.0f : dt, state.playing);
        // The ears: the player's camera in Play, else the editor camera.
        if (state.playing && !state.ejected) audio.set_listener(play.eye(), play.forward());
        else audio.set_listener(state.camera.position, lsd::editor::camera_forward(state.camera));
        audio.update(dt);
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
        for (lsd::u32 v = 0; v < state.viewport_count; ++v) {
            const lsd::f32* rect = state.viewport_rects[v];
            if (rect[2] < 8.0f || rect[3] < 8.0f) continue;
            const lsd::editor::EditorCamera& camera = state.viewport_count == 1 ? state.camera : state.viewport_camera(v);
            // ImGui works in window points; the swapchain is in pixels (differs on high-DPI screens).
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            device->set_viewport(rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y);
            lsd::editor::EditorCamera lens = camera;
            if (state.playing && !state.ejected) lens.view = lsd::editor::ViewType::Perspective;  // the player always sees in perspective
            const lsd::Mat4 proj = lsd::editor::camera_projection(lens, rect[2] / rect[3]);
            const lsd::Mat4 view = state.playing && !state.ejected ? lsd::look_at(play.eye(), play.eye() + play.forward(), lsd::Vec3{0, 0, 1}) : lsd::editor::camera_view(camera);
            const lsd::Mat4 view_proj = proj * view;
            if (state.show_grid && !state.game_view && !state.playing) device->draw_lines(grid.vertices, grid.vertex_count, view_proj.m);
            lsd::editor::LightView light_view;
            light_view.eye = state.playing && !state.ejected ? play.eye() : camera.position;
            light_view.forward = state.playing && !state.ejected ? play.forward() : lsd::editor::camera_forward(camera);
            light_view.fov_y = lens.fov_y;
            light_view.aspect = rect[2] / rect[3];
            const lsd::f32 pixels[4] = {rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y};
            draw_world(*device, view_proj, light_view, pixels, shadow_atlas, local_shadow_atlas, world, meshes, materials, state);
            effects->draw(*device, world, view_proj, view, light_view.eye, !state.playing || state.ejected);  // after the opaque meshes
        }
        device->render_debug_ui();
        if (frame == screenshot_frame && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl for now.");
        }
        device->end_frame();
        ++frame;
    }

    fs::remove(kLockPath, ignored);
    lsd::console::remove_all();  // the registrations point into state
    lsd::log::set_sink(nullptr, nullptr);
    std::snprintf(state.settings.last_level, sizeof state.settings.last_level, "%s", state.level_path);
    lsd::reflect::save_file(state.settings, kSettingsPath);
    play.stop();
    audio.shutdown();
    SDL_SetWindowRelativeMouseMode(window, false);
    if (gamepad) SDL_CloseGamepad(gamepad);
    lsd::render::destroy_lines(*device, grid);
    thumbnails.~ThumbnailCache();
    allocator.deallocate(thumbnail_memory);
    delete animations;
    delete effects;
    delete skinned_cache;
    delete texture_cache;
    device->destroy_texture(shadow_atlas);
    device->destroy_texture(local_shadow_atlas);
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
