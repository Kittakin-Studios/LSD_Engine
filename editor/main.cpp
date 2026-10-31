// LSD_Engine editor entry point: level editor (Viewport, Outliner, Details, Content Browser, Play).
#include "editor_ui.h"
#include "graph_editor.h"
#include "table_editor.h"
#include "mesh_cache.h"
#include "lighting.h"
#include "material_cache.h"
#include "animation_player.h"
#include "particle_editor.h"
#include "particle_player.h"
#include "packaging.h"
#include "navigation.h"
#include "ui_editor.h"

#include "lsd/asset/texture.h"
#include "lsd/foundation/profile.h"
#include "lsd/core/crash.h"
#include "lsd/ui/ui.h"
#include "skinned_cache.h"
#include "play_session.h"
#include "light_bake.h"
#include "sound_player.h"
#include "debug_draw.h"
#include "remote_console.h"
#include "source_control.h"
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <filesystem>
#include <new>
#include <string>

namespace {

namespace fs = std::filesystem;
using lsd::editor::EditorState;

constexpr char kContentDir[] = "Content";
constexpr char kDataDir[] = "DerivedData";
constexpr char kSettingsPath[] = "Saved/editor_settings.json";
constexpr char kUserSettingsPath[] = "Saved/GameUserSettings.json";  // the player's (also in a packaged game)
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
lsd::editor::BakedLighting g_level_lighting;  // the open level's Build Lighting result

// Build Lighting runs on its own thread; the editor keeps working and shows its progress.
struct LightingBuild {
    std::thread thread;
    std::atomic<lsd::u32> done{0};
    std::atomic<bool> finished{false};
    std::atomic<lsd::u32> total{0};
    lsd::u64 started = 0;
    char path[300] = "";
    lsd::editor::BakeScene scene;
    lsd::editor::BakedLighting result;
};
LightingBuild* g_lighting_build = nullptr;

// The level's baked lighting from its _BuiltData file (none: the Sky Light's flat ambient).
void load_level_lighting(const char* level_path) {
    const bool found = level_path && level_path[0] && lsd::editor::load_baked_lighting(lsd::editor::baked_lighting_path(level_path).c_str(), g_level_lighting);
    if (!found) g_level_lighting = {};
    lsd::editor::set_baked_lighting(found ? &g_level_lighting : nullptr);
}

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
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::shadow_atlas_size());
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
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::shadow_atlas_size());
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

// Frustum culling: false when the box (local bounds under `mvp`) is wholly outside one clip plane.
bool in_view(const lsd::Mat4& mvp, lsd::Vec3 lo, lsd::Vec3 hi) {
    int outside[6] = {};
    for (int c = 0; c < 8; ++c) {
        const lsd::Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
        const lsd::f32 x = mvp.at(0, 0) * p.x + mvp.at(0, 1) * p.y + mvp.at(0, 2) * p.z + mvp.at(0, 3);
        const lsd::f32 y = mvp.at(1, 0) * p.x + mvp.at(1, 1) * p.y + mvp.at(1, 2) * p.z + mvp.at(1, 3);
        const lsd::f32 z = mvp.at(2, 0) * p.x + mvp.at(2, 1) * p.y + mvp.at(2, 2) * p.z + mvp.at(2, 3);
        const lsd::f32 w = mvp.at(3, 0) * p.x + mvp.at(3, 1) * p.y + mvp.at(3, 2) * p.z + mvp.at(3, 3);
        outside[0] += x < -w;
        outside[1] += x > w;
        outside[2] += y < -w;
        outside[3] += y > w;
        outside[4] += z < 0.0f;  // 0..1 depth
        outside[5] += z > w;
    }
    for (int k : outside) {
        if (k == 8) return false;
    }
    return true;
}

// stat unit: CPU milliseconds of the frame's parts (smoothed when shown).
struct FrameTimer {
    lsd::u64 start = SDL_GetTicksNS();
    lsd::f32 lap() {
        const lsd::u64 now = SDL_GetTicksNS();
        const lsd::f32 ms = static_cast<lsd::f32>(static_cast<double>(now - start) * 1e-6);
        start = now;
        return ms;
    }
};

void draw_world(lsd::rhi::Device& device, const lsd::Mat4& view_proj, const lsd::editor::LightView& light_view, const lsd::f32 viewport[4],
                lsd::rhi::TextureHandle shadow_atlas, lsd::rhi::TextureHandle local_atlas, lsd::world::World& world, lsd::editor::MeshCache& meshes, lsd::editor::MaterialCache& materials,
                EditorState& stats, lsd::rhi::TextureHandle scene_target = {}, const lsd::f32* scene_clear = nullptr, bool hdr = true) {
    LSD_PROFILE_SCOPE("draw_world");
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    const lsd::Vec3 eye = light_view.eye;
    const bool skip_hidden = !stats.playing || stats.ejected;
    lsd::rhi::LightSetup lights = lsd::editor::world_lights(world, light_view, skip_hidden);
    const bool sun_shadows = lights.shadow_info[1] >= 0.0f && shadow_atlas.valid() && !stats.view_mode_unlit_or_wire();
    const bool local_shadows = lights.local_tiles[0][3] > 0.0f && local_atlas.valid() && !stats.view_mode_unlit_or_wire();
    if (sun_shadows) draw_shadows(device, lights, shadow_atlas, world, meshes, skip_hidden);
    if (local_shadows) draw_local_shadows(device, lights, local_atlas, world, meshes, skip_hidden);
    if (scene_target.valid()) {  // the post pass reads it: draw into the scene target (after the shadow passes)
        device.set_render_target(scene_target, scene_clear);
        lights.output[0] = hdr ? 1.0f : 0.0f;
    } else if (sun_shadows || local_shadows) {
        device.set_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
    device.set_shadow_map(sun_shadows ? shadow_atlas : lsd::rhi::TextureHandle{}, local_shadows ? local_atlas : lsd::rhi::TextureHandle{});
    device.set_lights(lights);
    lsd::rhi::SkyDraw sky;
    if (!stats.view_mode_unlit_or_wire() && lsd::editor::world_sky(world, view_proj, eye, skip_hidden, sky)) device.draw_sky(sky);  // behind everything
    stats.stat_draws = 0;
    stats.stat_triangles = 0;
    stats.stat_culled = 0;
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
        const lsd::world::Entity e = world.owner(type, i);
        if ((!stats.playing || stats.ejected) && world.hidden_in_hierarchy(e)) continue;  // hidden in the editor, not in the game
        const lsd::editor::CachedMesh* mesh = meshes.get(component->mesh);
        if (!mesh) continue;
        const lsd::Mat4 m = world.world_matrix(e);
        const lsd::Mat4 mvp = view_proj * m;
        if (!in_view(mvp, mesh->bounds_min, mesh->bounds_max)) {
            ++stats.stat_culled;
            continue;
        }
        const lsd::Mat4 inv = lsd::affine_inverse(m);  // normals use the inverse transpose (correct under non-uniform scale)
        lsd::rhi::MeshDraw draw{};
        draw.vertices = mesh->gpu.vertices;
        draw.indices = mesh->gpu.indices;
        draw.index_count = mesh->gpu.index_count;
        if (mesh->lod_count > 1) {  // distant meshes draw a simpler LOD
            const lsd::Vec3 half = (mesh->bounds_max - mesh->bounds_min) * 0.5f;
            const lsd::f32 scale = std::fmax(lsd::length(lsd::Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}),
                                             std::fmax(lsd::length(lsd::Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}), lsd::length(lsd::Vec3{m.at(0, 2), m.at(1, 2), m.at(2, 2)})));
            const lsd::Vec3 centre = lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f);
            const lsd::f32 distance = std::fmax(lsd::length(centre - eye), 0.001f);
            const lsd::u32 lod = mesh->pick_lod(lsd::length(half) * scale / distance, stats.force_lod);
            if (lod > 0) {
                draw.indices = mesh->lod_indices[lod];
                draw.index_count = mesh->lod_index_count[lod];
            }
        }
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
        draw.time = static_cast<lsd::f32>(SDL_GetTicks()) * 0.001f;  // Material Graph Time
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
        stats.stat_triangles += draw.index_count / 3;
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
        // Bind-pose bounds grown by half their size: animation moves limbs out of the rest pose.
        const lsd::Vec3 grow = (model->model->bounds_max - model->model->bounds_min) * 0.5f;
        if (!in_view(mvp, model->model->bounds_min - grow, model->model->bounds_max + grow)) {
            ++stats.stat_culled;
            continue;
        }
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
        draw.time = static_cast<lsd::f32>(SDL_GetTicks()) * 0.001f;
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
    console::add_variable("show.Navigation", &s.show_navigation, "Draw the navmesh (P)");
    console::add_variable("r.ForceLOD", &s.force_lod, "Draw every mesh at this LOD (-1: pick by distance)");
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
    console::add_command("particles.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hits[64];
        const lsd::usize count = st.registry->search(args, lsd::asset::AssetType::Data, nullptr, hits, 64);
        for (lsd::usize i = 0; i < count; ++i) {
            const lsd::asset::AssetRecord& r = st.registry->get(hits[i]);
            if (!lsd::editor::asset_matches(r, "ParticleSystem") || !st.particle_editor) continue;
            st.particle_editor->open(st, r);
            return;
        }
        LSD_LOG_WARN("editor", "particles.Open: no particle system named like '%s'", args);
    }, &s, "Open a particle system in the Particle Editor");
    console::add_command("graph.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Graph, nullptr, &hit, 1) == 1 || st.registry->search(args, lsd::asset::AssetType::Cue, nullptr, &hit, 1) == 1 ||
            st.registry->search(args, lsd::asset::AssetType::MaterialGraph, nullptr, &hit, 1) == 1 ||
            st.registry->search(args, lsd::asset::AssetType::BehaviorTree, nullptr, &hit, 1) == 1) {
            lsd::editor::open_graph(st, st.registry->get(hit));
        } else {
            LSD_LOG_WARN("editor", "graph.Open: no visual script or sound cue named like '%s'", args);
        }
    }, &s, "Open a visual script, sound cue or material graph in the graph editor");
    console::add_command("table.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Table, nullptr, &hit, 1) == 1) lsd::editor::open_table(st, st.registry->get(hit));
        else LSD_LOG_WARN("editor", "table.Open: no data table named like '%s'", args);
    }, &s, "Open a data table in the table editor");
    console::add_command("BuildLighting", [](void* user, const char*) { static_cast<EditorState*>(user)->build_lighting_requested = true; }, &s,
                         "Build > Build Lighting: bake the level's light probes");
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
    console::add_command("HighResShot", [](void* user, const char* args) {
        const int n = std::atoi(args);
        static_cast<EditorState*>(user)->high_res_shot = static_cast<lsd::u32>(n >= 1 && n <= 8 ? n : 2);
    }, &s, "HighResShot <1-8>: the main view at that many times its size, saved to Saved/Screenshots (PNG)");
    console::add_command("test.Smoke", [](void* user, const char* args) {
        const float seconds = static_cast<float>(std::atof(args));
        static_cast<EditorState*>(user)->smoke_test_seconds = seconds > 0.0f ? seconds : 5.0f;
    }, &s, "test.Smoke [seconds]: play this level and report the errors it logs");
    console::add_command("test.Unit", [](void* user, const char*) { static_cast<EditorState*>(user)->run_unit_tests = true; }, &s,
                         "Run the unit tests (ctest) in the background");
    console::add_command("validate", [](void* user, const char*) {
        auto& st = *static_cast<EditorState*>(user);
        const lsd::u32 n = lsd::editor::validate_assets(st.project, *st.registry);
        std::snprintf(st.status, sizeof st.status, "Asset validation: %u problem%s (Message Log)", n, n == 1 ? "" : "s");
    }, &s, "Validate Assets: missing references, oversize textures, naming");
    console::add_command("package", [](void* user, const char*) { static_cast<EditorState*>(user)->package_requested = true; }, &s,
                         "Package the project into Build/<Project Name>/ (File > Package Project)");
    console::add_command("debug.Crash", [](void*, const char*) {  // tests the crash reporter
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }, &s, "Crash on purpose (writes a crash report to Saved/Crashes)");
    console::add_command("ui.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::UiDocument, nullptr, &hit, 1) == 1 || st.registry->search(args, lsd::asset::AssetType::UiStyle, nullptr, &hit, 1) == 1) {
            lsd::editor::open_ui_editor(st, st.registry->get(hit));
        } else {
            LSD_LOG_WARN("editor", "ui.Open: no UI document or style named like '%s'", args);
        }
    }, &s, "Open a UI document (.rml) or style (.rcss) in the UI window");
    console::add_command("ui.Keys", [](void*, const char* args) {  // tests: "down down enter", 0.8 s later
        static char keys[128];
        std::snprintf(keys, sizeof keys, "%s", args);
        SDL_AddTimer(800, [](void*, SDL_TimerID, Uint32) -> Uint32 {
            char copy[128];
            std::snprintf(copy, sizeof copy, "%s", keys);
            for (char* word = std::strtok(copy, " "); word; word = std::strtok(nullptr, " ")) {
                const SDL_Keycode k = std::strcmp(word, "down") == 0 ? SDLK_DOWN : std::strcmp(word, "up") == 0 ? SDLK_UP : std::strcmp(word, "enter") == 0 ? SDLK_RETURN : SDLK_UNKNOWN;
                for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
                    SDL_Event e{};
                    e.type = type;
                    e.key.key = k;
                    e.key.down = type == SDL_EVENT_KEY_DOWN;
                    SDL_PushEvent(&e);
                }
            }
            return 0;
        }, nullptr);
    }, &s, "ui.Keys <down|up|enter ...>: keys for the game UI, 0.8 s later (tests)");
    console::add_command("ui.Click", [](void*, const char* args) {  // tests: a real click through SDL's queue
        float* at = new float[2];  // each click keeps its own position (several can be queued)
        if (std::sscanf(args, "%f %f", &at[0], &at[1]) != 2) {
            delete[] at;
            return;
        }
        SDL_AddTimer(800, [](void* user, SDL_TimerID, Uint32) -> Uint32 {  // after Play has started and the UI is up
        const float* at = static_cast<const float*>(user);
        const float x = at[0], y = at[1];
        delete[] at;
        int window_count = 0;
        SDL_Window** windows = SDL_GetWindows(&window_count);
        const SDL_WindowID window = windows && window_count > 0 ? SDL_GetWindowID(windows[0]) : 0;  // ImGui ignores events for no window
        SDL_free(windows);
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = window;
        e.motion.x = x;
        e.motion.y = y;
        SDL_PushEvent(&e);
        for (const SDL_EventType type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            e = SDL_Event{};
            e.type = type;
            e.button.windowID = window;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.x = x;
            e.button.y = y;
            SDL_PushEvent(&e);
        }
        return 0;
        }, at);
    }, &s, "ui.Click <x> <y>: click the game UI at window pixels, 0.8 s later (tests)");
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
    load_level_lighting(nullptr);
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
    load_level_lighting(s.level_path);
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
    bool game_arg = false;             // -game: run as the game (as a package does), no editor
    const char* select_arg = nullptr;  // -select=<object name>: select it on startup (screenshots)
    int remote_arg = 0;                // -remote[=port]: listen for the editor's Remote Console
    float smoke_arg = 0.0f;            // -smoketest=<seconds>: play the level that long, exit 0 if no errors (CI)
    long vram_arg = 0;                 // -vram=<MB>: pretend the card has that much video memory (tests the scaling)
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
        else if (std::strcmp(argv[i], "-game") == 0) game_arg = true;
        else if (std::strncmp(argv[i], "-vram=", 6) == 0) vram_arg = std::atol(argv[i] + 6);
        else if (std::strcmp(argv[i], "-remote") == 0) remote_arg = lsd::editor::remote::kDefaultPort;
        else if (std::strncmp(argv[i], "-remote=", 8) == 0) remote_arg = std::atoi(argv[i] + 8);
        else if (std::strncmp(argv[i], "-smoketest=", 11) == 0) smoke_arg = static_cast<float>(std::atof(argv[i] + 11));
        else if (std::strncmp(argv[i], "-select=", 8) == 0) select_arg = argv[i] + 8;
        else if (std::strncmp(argv[i], "-exec=", 6) == 0 && exec_count < 8) exec_args[exec_count++] = argv[i] + 6;
        else LSD_LOG_WARN("editor", "Unknown argument: %s", argv[i]);
    }

    // A packaged game: game.json next to the exe. Its folder is the project (Content/, Config/...).
    bool game_mode = game_arg;
    if (const char* base = SDL_GetBasePath(); base && fs::exists(fs::path(base) / lsd::editor::kGameMarker)) {
        std::error_code cwd_error;
        fs::current_path(base, cwd_error);
        game_mode = true;
    }
    // The log on disk and crash reports (Saved/Logs, Saved/Crashes), like Unreal's Saved folder.
    std::error_code log_error;
    fs::create_directories("Saved/Logs", log_error);
    lsd::log::open_file(game_mode ? "Saved/Logs/Game.log" : "Saved/Logs/Editor.log");
    char last_crash[300];
    lsd::install_crash_reporting("Saved/Crashes", last_crash, sizeof last_crash);
    LSD_LOG_INFO("editor", game_mode ? "LSD_Engine 0.1.0 (game)" : "LSD_Engine Editor 0.1.0");
    if (last_crash[0]) LSD_LOG_WARN("editor", "The last session crashed: the dump and its log are in %s", last_crash);
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
    lsd::ui::init(*device);  // game UI (RmlUi): after the device's UI heap, before any texture

    // Placement-allocated: the state holds big buffers, and GPU owners must go before the device.
    auto* state_memory = allocator.allocate(sizeof(EditorState), alignof(EditorState));
    EditorState& state = *new (state_memory) EditorState();
    lsd::editor::load_theme(state.theme);
    lsd::editor::apply_theme(state.theme);
    lsd::reflect::load_file(state.settings, kSettingsPath);
    lsd::reflect::load_file(state.project, "Config/project.json");
    state.game_mode = game_mode;
    if (game_mode) SDL_SetWindowTitle(window, state.project.project_name);

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
    // The window icon (Project Settings > Platform), editor and game alike.
    lsd::asset::Guid icon_guid;
    if (const lsd::asset::AssetRecord* icon = state.project.icon[0] && lsd::asset::parse_guid(state.project.icon, icon_guid) ? registry.find(icon_guid) : nullptr) {
        char icon_path[300];
        registry.source_path(*icon, icon_path, sizeof icon_path);
        lsd::asset::TextureData pixels(allocator);
        if (lsd::asset::load_texture(icon_path, false, 256, pixels)) {
            if (SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(pixels.width), static_cast<int>(pixels.height), SDL_PIXELFORMAT_ABGR8888,
                                                             const_cast<lsd::u8*>(pixels.mip(0)), static_cast<int>(pixels.width * 4))) {
                SDL_SetWindowIcon(window, surface);
                SDL_DestroySurface(surface);
            }
        }
    }
    LSD_LOG_INFO("editor", "%zu assets in %s/", registry.count(), kContentDir);
    state.registry = &registry;

    lsd::world::World world(allocator);
    lsd::editor::Transactions history(allocator);
    auto* mesh_memory = allocator.allocate(sizeof(lsd::editor::MeshCache), alignof(lsd::editor::MeshCache));
    lsd::editor::MeshCache& meshes = *new (mesh_memory) lsd::editor::MeshCache(*device, allocator, registry, kDataDir);
    auto* texture_cache = new lsd::editor::TextureCache(*device, registry);  // deleted before the device
    // Video memory: smaller textures and shadow atlases on cards with little of it (instead of failing).
    {
        const lsd::u64 budget = vram_arg > 0 ? static_cast<lsd::u64>(vram_arg) * 1024ull * 1024ull : device->query_memory_budget().budget_bytes;
        const lsd::u64 gib = 1024ull * 1024ull * 1024ull;
        if (budget && budget < gib * 5 / 2) {
            texture_cache->set_size_cap(1024);
            lsd::editor::set_shadow_atlas_size(2048);
        } else if (budget && budget < gib * 5) {
            texture_cache->set_size_cap(2048);
        }
        LSD_LOG_INFO("editor", "Video memory budget %.1f GB: textures up to %s, shadow atlas %u", static_cast<double>(budget) / static_cast<double>(gib),
                     budget && budget < gib * 5 / 2 ? "1024" : budget && budget < gib * 5 ? "2048" : "full size", lsd::editor::shadow_atlas_size());
    }
    const lsd::rhi::TextureHandle shadow_atlas = device->create_shadow_map(lsd::editor::shadow_atlas_size());
    const lsd::rhi::TextureHandle local_shadow_atlas = device->create_shadow_map(lsd::editor::shadow_atlas_size());
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
    if (!game_mode) lsd::editor::source_control::start();  // Git badges in the Content Browser
    state.textures = texture_cache;
    state.skinned = skinned_cache;
    state.device = device;

    const char* startup_level = level_arg              ? level_arg
                                : game_mode              ? state.project.game_default_level
                                : state.settings.last_level[0] ? state.settings.last_level
                                                         : state.project.editor_startup_level;
    if (!startup_level[0] || !fs::exists(startup_level) || !open_level(state, startup_level)) new_level(state);

    // Crash recovery: a lock file left behind means the last session did not exit cleanly.
    if (!game_mode && fs::exists(kLockPath)) {
        const std::string autosave = autosave_path(state.level_path);
        std::error_code error;
        const bool newer = fs::exists(autosave) && (!state.level_path[0] || fs::last_write_time(autosave, error) > fs::last_write_time(state.level_path, error));
        if (newer && max_frames < 0) std::snprintf(state.recovery_path, sizeof state.recovery_path, "%s", autosave.c_str());
    }
    if (FILE* lock = game_mode ? nullptr : std::fopen(kLockPath, "wb")) std::fclose(lock);
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
    state.play_requested = play_arg || game_mode;
    state.smoke_test_seconds = smoke_arg;
    // Smoke tests: errors logged while the level plays; the result is logged (and the exit code with -smoketest).
    lsd::u64 smoke_started = 0, smoke_errors = 0;
    int exit_code = 0;
    std::thread unit_tests;
    register_console(state);
    for (lsd::u32 slot = 0; select_arg && slot < world.slot_count(); ++slot) {
        const lsd::world::Entity e = world.entity_at_slot(slot);
        if (e.valid() && std::strcmp(world.name(e), select_arg) == 0) {
            state.selected = e;
            state.selection[state.selection_count++] = e;
        }
    }

    lsd::render::GridColors current_grid = grid_colors(state.theme);
    lsd::render::GpuLines grid = lsd::render::create_grid(*device, allocator, current_grid);
    // Scene targets for the post-process pass, one per viewport, remade when its size changes.
    struct SceneTargets {
        lsd::rhi::TextureHandle target[4];
        lsd::u32 size[4][2] = {};
        lsd::rhi::TextureHandle get(lsd::rhi::Device& d, lsd::u32 v, lsd::u32 w, lsd::u32 h) {
            if (v >= 4 || !w || !h) return {};
            if (!target[v].valid() || size[v][0] != w || size[v][1] != h) {
                if (target[v].valid()) d.destroy_texture(target[v]);
                target[v] = d.create_render_target(w, h);
                size[v][0] = w;
                size[v][1] = h;
            }
            return target[v];
        }
    } scene_targets;
    struct TaaHistory {  // per viewport: last frame's view-projection (TAA reprojects into it)
        lsd::Mat4 view_proj;
        lsd::u32 frame = 0;
        bool valid = false;
    } taa_history[4];
    auto* particle_editor = new lsd::editor::ParticleEditor(*device, allocator, *effects, grid);  // previews with the level's grid
    state.particle_editor = particle_editor;
    for (int i = 0; i < exec_count; ++i) lsd::console::execute(exec_args[i]);
    if (remote_arg > 0 && remote_arg < 65536) lsd::editor::remote::listen(static_cast<lsd::u16>(remote_arg));

    lsd::audio::AudioSystem audio;
    audio.init(!no_sound);
    lsd::editor::SoundPlayer sounds;
    sounds.bind(&audio, &registry);
    state.sounds = &sounds;
    lsd::editor::PlaySession play;
    play.set_sounds(&sounds);
    auto* game_ui = new lsd::ui::UiContext("game");  // documents over the player's view; deleted before the device
    play.set_ui(game_ui);
    // The player's settings: loaded (the first time: the quality this graphics card suits) and applied.
    lsd::editor::GameUserSettings user_settings;
    if (!lsd::reflect::load_file(user_settings, kUserSettingsPath)) {
        user_settings.quality = static_cast<lsd::u32>(lsd::rhi::recommend_preset(device->adapter(), info.total_ram_bytes));
        LSD_LOG_INFO("editor", "Graphics quality: %s (recommended for this PC)", lsd::rhi::to_string(static_cast<lsd::rhi::QualityPreset>(user_settings.quality)));
    }
    auto apply_user_settings = [&] {
        lsd::editor::set_shadow_quality(user_settings.quality);
        sounds.set_class_volume("Master", user_settings.master_volume);
        sounds.set_class_volume("Music", user_settings.music_volume, false);  // projects without these classes: nothing to set
        sounds.set_class_volume("SFX", user_settings.effects_volume, false);
        if (game_mode) SDL_SetWindowFullscreen(window, user_settings.fullscreen);
    };
    apply_user_settings();
    play.set_user_settings(&user_settings);
    // Localisation: the project's string table in the player's language; UI documents translate keys.
    lsd::asset::Localization localization(allocator);
    {
        lsd::asset::Guid table;
        if (const lsd::asset::AssetRecord* r = state.project.string_table[0] && lsd::asset::parse_guid(state.project.string_table, table) ? registry.find(table) : nullptr) {
            char path[300];
            registry.source_path(*r, path, sizeof path);
            if (!localization.load(path)) LSD_LOG_WARN("editor", "Could not read the string table %s", path);
        }
        localization.set_language(user_settings.language[0] ? user_settings.language : state.project.default_language);
    }
    play.set_localization(&localization);
    lsd::ui::set_translator([](void* user, const char* text) { return static_cast<const lsd::asset::Localization*>(user)->find(text); }, &localization);
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
    lsd::rhi::TextureHandle shot_target, shot_scene;  // HighResShot, read back after the frame
    lsd::f32 shot_size[2] = {};
    lsd::u64 last_scan = SDL_GetTicks();
    lsd::u64 last_tick = SDL_GetTicksNS();
    for (long frame = 0; running && frame != max_frames;) {
        SDL_Event event;
        lsd::editor::remote::poll();
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (state.playing && !state.ejected && play.ui_cursor()) {  // a menu has the mouse, keys and the gamepad's D-pad / A
                game_ui->process_event(event);
                if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    switch (event.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_DPAD_UP: game_ui->navigate(0, -1); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: game_ui->navigate(0, 1); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: game_ui->navigate(-1, 0); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: game_ui->navigate(1, 0); break;
                        case SDL_GAMEPAD_BUTTON_SOUTH: game_ui->activate(); break;
                        default: break;
                    }
                }
            }
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

        // The navmesh preview (P): rebuilt when switched on, then about once a second (Play shows its own).
        static lsd::u64 last_nav = 0;
        static bool was_showing = false;
        if (state.show_navigation && (!was_showing || SDL_GetTicks() - last_nav > 1000)) {
            static lsd::ai::NavMesh preview;
            if (!state.playing) lsd::editor::build_level_navmesh(world, meshes, preview);
            const lsd::ai::NavMesh& nav = state.playing ? play.navmesh() : preview;
            state.nav_line_count = nav.debug_lines(state.nav_lines, sizeof state.nav_lines / sizeof state.nav_lines[0]);
            last_nav = SDL_GetTicks();
        }
        was_showing = state.show_navigation;

        FrameTimer timer;
        device->debug_ui_new_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        {
            LSD_PROFILE_SCOPE("editor ui");
            if (game_mode) lsd::editor::draw_game(state);
            else lsd::editor::draw_editor(state);
            ImGui::Render();
        }
        state.stat_ui_ms = timer.lap();
        if (state.package_requested) {
            state.package_requested = false;
            lsd::editor::package_project(state.project, registry, state.status, sizeof state.status);
        }
        if (state.build_lighting_requested) {  // Build > Build Lighting: gather here, trace on a thread
            state.build_lighting_requested = false;
            if (!state.level_path[0]) {
                std::snprintf(state.status, sizeof state.status, "Build Lighting: save the level first (the result goes next to it)");
            } else {
                auto* build = new LightingBuild;
                if (!lsd::editor::gather_bake_scene(world, registry, kDataDir, allocator, build->scene)) {
                    std::snprintf(state.status, sizeof state.status, "Build Lighting: the level has no static meshes to bake against");
                    delete build;
                } else {
                    std::snprintf(build->path, sizeof build->path, "%s", lsd::editor::baked_lighting_path(state.level_path).c_str());
                    build->started = SDL_GetTicks();
                    LSD_LOG_INFO("lighting", "Building lighting: %zu triangles", build->scene.positions.size() / 3);
                    build->thread = std::thread([build] {
                        lsd::editor::bake_lighting(build->scene, build->result, &build->done, &build->total);
                        build->finished = true;
                    });
                    g_lighting_build = build;
                    state.build_lighting_progress = 0.0f;
                }
            }
        }
        if (g_lighting_build) {
            LightingBuild& build = *g_lighting_build;
            const lsd::u32 total = build.total.load() ? build.total.load() : 1u;
            state.build_lighting_progress = static_cast<float>(build.done.load()) / static_cast<float>(total);
            std::snprintf(state.status, sizeof state.status, "Building lighting... %d%%", static_cast<int>(state.build_lighting_progress * 100.0f));
            if (build.finished) {
                build.thread.join();
                g_level_lighting = std::move(build.result);
                lsd::editor::set_baked_lighting(&g_level_lighting);
                const bool saved = lsd::editor::save_baked_lighting(build.path, g_level_lighting);
                std::snprintf(state.status, sizeof state.status, saved ? "Lighting built: %u probes in %.1f s (%s)" : "Lighting built (%u probes, %.1f s) but %s could not be written",
                              g_level_lighting.probe_count(), static_cast<double>(SDL_GetTicks() - build.started) / 1000.0, build.path);
                LSD_LOG_INFO("lighting", "%s", state.status);
                delete g_lighting_build;
                g_lighting_build = nullptr;
                state.build_lighting_progress = -1.0f;
            }
        }

        // ---- Requests from the UI ----
        if (state.smoke_test_seconds > 0.0f && !smoke_started && !state.playing) {
            smoke_errors = lsd::log::error_count();
            smoke_started = SDL_GetTicks();
            state.play_requested = true;
            LSD_LOG_INFO("test", "Smoke test: playing %s for %.0f s", state.level_path[0] ? state.level_path : "the level", static_cast<double>(state.smoke_test_seconds));
        }
        if (smoke_started && SDL_GetTicks() - smoke_started > static_cast<lsd::u64>(state.smoke_test_seconds * 1000.0f)) {
            const lsd::u64 errors = lsd::log::error_count() - smoke_errors;
            if (errors) LSD_LOG_ERROR("test", "Smoke test FAILED: %llu error%s while playing (see above)", static_cast<unsigned long long>(errors), errors == 1 ? "" : "s");
            else LSD_LOG_INFO("test", "Smoke test passed: no errors in %.0f s of play", static_cast<double>(state.smoke_test_seconds));
            std::snprintf(state.status, sizeof state.status, "Smoke test %s", errors ? "FAILED (Output Log)" : "passed");
            state.stop_requested = true;
            state.smoke_test_seconds = 0.0f;
            smoke_started = 0;
            exit_code = errors ? 1 : 0;
            if (smoke_arg > 0.0f) running = false;  // -smoketest: done
        }
        if (state.run_unit_tests) {
            state.run_unit_tests = false;
            if (unit_tests.joinable()) unit_tests.join();
            // shortcut: runs the build tree's ctest (a developer setup); packaged games have no tests.
            unit_tests = std::thread([] {
                LSD_LOG_INFO("test", "Running unit tests (ctest)...");
                const int result = std::system("ctest --preset windows-msvc --output-on-failure > Saved/Logs/UnitTests.log 2>&1");
                std::string output;
                if (FILE* f = std::fopen("Saved/Logs/UnitTests.log", "rb")) {
                    char buffer[512];
                    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) output.append(buffer, n);
                    std::fclose(f);
                }
                if (result == 0) LSD_LOG_INFO("test", "Unit tests passed (Saved/Logs/UnitTests.log)");
                else if (output.find("not recognized") != std::string::npos || output.find("not found") != std::string::npos)
                    LSD_LOG_WARN("test", "ctest was not found: start the editor from a Visual Studio developer prompt (or put CMake on the PATH)");
                else LSD_LOG_ERROR("test", "Unit tests FAILED: see Saved/Logs/UnitTests.log");
            });
        }
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
        if (!game_mode && autosave_ms > 0.0f && SDL_GetTicks() - last_autosave > static_cast<lsd::u64>(autosave_ms)) {
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
            lsd::editor::debug_draw::clear();
            state.mouse_captured = false;
            state.paused = state.ejected = false;
            play.stop();
            LSD_LOG_INFO("editor", "Stop");
        }
        if (game_mode && !state.playing && !state.play_requested) running = false;
        state.stop_requested = false;
        if (state.playing && play.ui_cursor()) state.mouse_captured = false;  // a menu: the cursor is free
        if (play.take_settings_changed()) {  // an options menu changed them
            apply_user_settings();
            lsd::reflect::save_file(user_settings, kUserSettingsPath);
        }
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
            LSD_PROFILE_SCOPE("game");
            lsd::editor::debug_draw::tick(state.paused ? 0.0f : dt);  // the UI has drawn them; one-frame shapes go before scripts add new ones
            if (!state.paused || state.step_requested) play.update(world, devices, state.paused ? 1.0f / 60.0f : dt);
            state.step_requested = false;
            state.player_feet = play.feet();
        }
        // Behaviour tree live debugging: the open tree's running nodes glow.
        {
            lsd::asset::Guid tree;
            lsd::u32 active[64];
            char guid[33];
            const bool shown = state.playing && lsd::editor::open_behavior_tree(tree);
            if (shown) lsd::asset::to_string(tree, guid);
            lsd::editor::set_graph_active_nodes(active, shown ? play.active_bt_nodes(guid, active, 64) : 0);
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
        if (!game_mode && std::strcmp(wanted_title, title) != 0) {
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

        state.stat_game_ms = timer.lap();  // play, animation, particles, audio, file checks
        const lsd::LinearColor& bg = state.theme.viewport_background;
        const lsd::f32 clear_color[4] = {bg.r, bg.g, bg.b, 1.0f};
        thumbnails.release_temporaries();
        device->begin_frame(clear_color);
        thumbnails.render(clear_color);
        particle_editor->render(*device, clear_color);
        lsd::editor::render_graph_preview(state, clear_color);
        lsd::editor::render_ui_editor(*device, clear_color);
        for (lsd::u32 v = 0; v < state.viewport_count; ++v) {
            const lsd::f32* rect = state.viewport_rects[v];
            if (rect[2] < 8.0f || rect[3] < 8.0f) continue;
            const lsd::editor::EditorCamera& camera = state.viewport_count == 1 ? state.camera : state.viewport_camera(v);
            // ImGui works in window points; the swapchain is in pixels (differs on high-DPI screens).
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            device->set_viewport(rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y);
            lsd::editor::EditorCamera lens = camera;
            if (state.playing && !state.ejected) lens.view = lsd::editor::ViewType::Perspective;  // the player always sees in perspective
            lsd::Mat4 proj = lsd::editor::camera_projection(lens, rect[2] / rect[3]);
            const lsd::Mat4 view = state.playing && !state.ejected ? lsd::look_at(play.eye(), play.eye() + play.forward(), lsd::Vec3{0, 0, 1}) : lsd::editor::camera_view(camera);
            lsd::Mat4 view_proj = proj * view;
            state.drawn_view_proj[v] = view_proj;
            lsd::editor::LightView light_view;
            light_view.eye = state.playing && !state.ejected ? play.eye() : camera.position;
            light_view.forward = state.playing && !state.ejected ? play.forward() : lsd::editor::camera_forward(camera);
            light_view.fov_y = lens.fov_y;
            light_view.aspect = rect[2] / rect[3];
            const lsd::f32 pixels[4] = {rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y};
            // Post-processing (a Post Process Volume applies): the scene goes into a target the post pass reads.
            lsd::rhi::PostDraw post;
            const bool use_post = !state.view_mode_unlit_or_wire() && lsd::editor::world_post(world, light_view.eye, !state.playing || state.ejected, post);
            if (use_post && lens.view == lsd::editor::ViewType::Perspective) {  // depth effects (AO, DOF, TAA) need the projection
                post.proj[0] = proj.at(0, 0);
                post.proj[1] = proj.at(1, 1);
                post.proj[2] = proj.at(2, 2);
                post.proj[3] = proj.at(2, 3);
            }
            const bool taa = post.taa && post.proj[0] != 0.0f && v < 4;
            if (taa) {
                // Last frame's clip space from this one's (without the jitter), then a sub-pixel jitter (Halton 2, 3).
                post.taa_reset = !taa_history[v].valid;
                const lsd::Mat4 reproject = taa_history[v].view_proj * lsd::inverse(view_proj);
                std::memcpy(post.reproject, reproject.m, sizeof post.reproject);
                taa_history[v].view_proj = view_proj;
                taa_history[v].valid = true;
                const lsd::u32 n = ++taa_history[v].frame % 8 + 1;
                auto halton = [](lsd::u32 i, lsd::u32 base) {
                    lsd::f32 f = 1.0f, r = 0.0f;
                    for (; i; i /= base) {
                        f /= static_cast<lsd::f32>(base);
                        r += f * static_cast<lsd::f32>(i % base);
                    }
                    return r;
                };
                proj.at(0, 2) += (halton(n, 2) - 0.5f) * 2.0f / pixels[2];
                proj.at(1, 2) += (halton(n, 3) - 0.5f) * 2.0f / pixels[3];
                view_proj = proj * view;
            } else if (v < 4) {
                taa_history[v].valid = false;
            }
            if (v == 0 && state.high_res_shot) {  // HighResShot: this view again, bigger, into targets read after the frame
                const lsd::u32 n = state.high_res_shot;
                state.high_res_shot = 0;
                lsd::u32 w = static_cast<lsd::u32>(pixels[2]) * n, h = static_cast<lsd::u32>(pixels[3]) * n;
                while (w > 8192 || h > 8192) {
                    w /= 2;
                    h /= 2;
                }
                const lsd::f32 full[4] = {0.0f, 0.0f, static_cast<lsd::f32>(w), static_cast<lsd::f32>(h)};
                shot_target = device->create_render_target(w, h);
                shot_size[0] = static_cast<lsd::f32>(w);
                shot_size[1] = static_cast<lsd::f32>(h);
                if (shot_target.valid() && use_post) {
                    shot_scene = device->create_render_target(w, h);
                    lsd::f32 encoded[4] = {0, 0, 0, 1};
                    for (int c = 0; c < 3; ++c) {
                        const lsd::f32 lin = clear_color[c] <= 0.04045f ? clear_color[c] / 12.92f : std::pow((clear_color[c] + 0.055f) / 1.055f, 2.4f);
                        encoded[c] = std::sqrt(lin / (1.0f + lin));
                    }
                    draw_world(*device, view_proj, light_view, full, shadow_atlas, local_shadow_atlas, world, meshes, materials, state, shot_scene, encoded, true);
                    effects->draw(*device, world, view_proj, view, light_view.eye, !state.playing || state.ejected);
                    lsd::rhi::PostDraw shot = post;
                    shot.taa = false;  // one frame: no history
                    shot.scene = shot_scene;
                    shot.output = shot_target;
                    for (int c = 0; c < 4; ++c) shot.viewport[c] = full[c];
                    device->draw_post(shot);
                } else if (shot_target.valid()) {
                    draw_world(*device, view_proj, light_view, full, shadow_atlas, local_shadow_atlas, world, meshes, materials, state, shot_target, clear_color, false);
                    effects->draw(*device, world, view_proj, view, light_view.eye, !state.playing || state.ejected);
                }
                device->set_render_target({}, nullptr);
                device->set_viewport(pixels[0], pixels[1], pixels[2], pixels[3]);
            }
            const lsd::rhi::TextureHandle scene = use_post ? scene_targets.get(*device, v, static_cast<lsd::u32>(pixels[2]), static_cast<lsd::u32>(pixels[3])) : lsd::rhi::TextureHandle{};
            lsd::f32 scene_clear[4] = {0, 0, 0, 1};
            for (int c = 0; c < 3; ++c) {  // the background, as the scene target stores colour (linear, range-compressed)
                const lsd::f32 srgb = clear_color[c];
                const lsd::f32 lin = srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
                scene_clear[c] = std::sqrt(lin / (1.0f + lin));
            }
            draw_world(*device, view_proj, light_view, pixels, shadow_atlas, local_shadow_atlas, world, meshes, materials, state, scene, scene_clear);
            if (state.show_grid && !state.game_view && !state.playing) device->draw_lines(grid.vertices, grid.vertex_count, view_proj.m);
            effects->draw(*device, world, view_proj, view, light_view.eye, !state.playing || state.ejected);  // after the opaque meshes
            if (scene.valid()) {
                post.scene = scene;
                for (int c = 0; c < 4; ++c) post.viewport[c] = pixels[c];
                device->draw_post(post);
            }
            if (state.playing && !state.ejected && v == 0) {  // the game's UI over the player's view
                play.place_ui_followers(world, view_proj, pixels);
                game_ui->update(pixels);
                game_ui->render(*device);
            }
        }
        device->render_debug_ui();
        state.stat_render_ms = timer.lap();  // CPU side of drawing (the GPU runs alongside)
        LSD_PROFILE_FRAME();
        if (frame == screenshot_frame && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl for now.");
        }
        device->end_frame();
        ++frame;
        if (shot_target.valid()) {  // HighResShot: read back, save, free
            const lsd::u32 w = static_cast<lsd::u32>(shot_size[0]), h = static_cast<lsd::u32>(shot_size[1]);
            std::vector<lsd::u8> rgba(static_cast<size_t>(w) * h * 4);
            fs::create_directories("Saved/Screenshots", ignored);
            char path[128];
            std::snprintf(path, sizeof path, "Saved/Screenshots/HighResShot_%llu.png", static_cast<unsigned long long>(SDL_GetTicks()));
            if (device->read_render_target(shot_target, rgba.data())) {
                for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;  // opaque (the post pass keeps luma in alpha)
                if (lsd::asset::write_png(path, w, h, rgba.data())) {
                    LSD_LOG_INFO("editor", "Screenshot: %s (%u x %u)", path, w, h);
                    std::snprintf(state.status, sizeof state.status, "Screenshot saved: %s", path);
                }
            } else {
                LSD_LOG_WARN("editor", "HighResShot: this graphics backend cannot read targets back");
            }
            device->destroy_texture(shot_target);
            if (shot_scene.valid()) device->destroy_texture(shot_scene);
            shot_target = shot_scene = {};
        }
    }

    if (unit_tests.joinable()) unit_tests.join();
    if (!game_mode) fs::remove(kLockPath, ignored);
    lsd::console::remove_all();  // the registrations point into state
    lsd::editor::shutdown_graph_editor();
    lsd::log::set_sink(nullptr, nullptr);
    lsd::log::open_file(nullptr);
    std::snprintf(state.settings.last_level, sizeof state.settings.last_level, "%s", state.level_path);
    if (!game_mode) lsd::reflect::save_file(state.settings, kSettingsPath);
    play.stop();
    audio.shutdown();
    SDL_SetWindowRelativeMouseMode(window, false);
    if (gamepad) SDL_CloseGamepad(gamepad);
    lsd::render::destroy_lines(*device, grid);
    for (const lsd::rhi::TextureHandle t : scene_targets.target) {
        if (t.valid()) device->destroy_texture(t);
    }
    lsd::editor::source_control::stop();
    if (g_lighting_build) {  // let a running bake finish (it cannot be interrupted)
        g_lighting_build->thread.join();
        delete g_lighting_build;
        g_lighting_build = nullptr;
    }
    lsd::editor::set_baked_lighting(nullptr);
    lsd::editor::remote::close();
    thumbnails.~ThumbnailCache();
    allocator.deallocate(thumbnail_memory);
    delete particle_editor;
    play.set_ui(nullptr);  // ~PlaySession stops Play again: it must not reach the deleted context
    lsd::ui::set_translator(nullptr, nullptr);
    lsd::editor::shutdown_ui_editor();
    delete game_ui;
    lsd::ui::shutdown();  // frees its textures: before the device
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
    return exit_code;
}
