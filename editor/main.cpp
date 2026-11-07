// LSD_Engine editor entry point: level editor (Viewport, Outliner, Details, Content Browser, Play).
#include "editor_ui.h"
#include "keymap.h"
#include "assistant.h"
#include "assistant_lessons.h"
#include "templates.h"
#include "editor_modes.h"
#include "paper2d.h"
#include "graph_editor.h"
#include "table_editor.h"
#include "mesh_cache.h"
#include "lighting.h"
#include "scalability.h"
#include "loc_gather.h"
#include "material_cache.h"
#include "animation_player.h"
#include "particle_editor.h"
#include "particle_player.h"
#include "packaging.h"
#include "navigation.h"
#include "ui_editor.h"
#include "timeline_editor.h"
#include "mesh_editor.h"
#include "mesh_materials.h"
#include "lua_debugger.h"
#include "prefab_editor.h"

#include "lsd/asset/texture.h"
#include "lsd/foundation/profile.h"
#include "lsd/core/crash.h"
#include "lsd/ui/ui.h"
#include "skinned_cache.h"
#include "play_session.h"
#include "decals.h"
#include "light_bake.h"
#include "live_link.h"
#include "sound_player.h"
#include "debug_draw.h"
#include "remote_console.h"
#include "game_module.h"
#include "source_control.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/asset/data_asset.h"
#include "lsd/core/console.h"
#include "lsd/input/input.h"
#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/world/curve.h"
#include "lsd/world/user_types.h"
#include "lsd/world/timeline.h"
#include "lsd/script/graph.h"
#include "lsd/render/material.h"
#include "lsd/audio/audio.h"
#include "lsd/foundation/system_info.h"
#include "lsd/render/grid.h"
#include "lsd/render/mesh.h"
#include "lsd/render/occlusion.h"
#include "lsd/render/reflection.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/components.h"
#include "lsd/world/level.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"

#include <unordered_map>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <new>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>

bool g_gpu_culling = true;  // r.GPUCulling
bool g_occlusion_culling = true;  // r.OcclusionCulling
bool g_gpu_mark_shadows = false;  // the next sun shadow pass ends GPU timing section 1
// The selected objects' draws of the view being drawn: outlined after its post pass (Unreal's orange outline).
std::vector<lsd::rhi::MeshDraw> g_outline;
constexpr lsd::f32 kOutlineColor[4] = {1.0f, 0.55f, 0.08f, 1.0f};

// input.Hold <key> <seconds>: keys held for the game as if pressed (tests and the MCP bridge drive Play).
lsd::u64 g_held_until[SDL_SCANCODE_COUNT] = {};
lsd::u64 g_mouse_held_until[3] = {};  // input.Hold on Left / Middle / Right Mouse Button

namespace {

bool g_screenshot = false;  // the screenshot command: capture the next frame

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
lsd::editor::MaterialCache* g_materials = nullptr;  // the shadow passes ask it for Masked materials

// A Masked material's cut-out for the shadow passes (its base colour texture and cutoff); false when it
// casts a solid shadow (opaque, a material graph, translucent).
bool shadow_cutout(const char* material, const lsd::LinearColor& tint, lsd::rhi::ShadowCutout& out) {
    if (!g_materials || !material || !material[0]) return false;
    lsd::rhi::MeshDraw look{};
    look.color[0] = look.color[1] = look.color[2] = 1.0f;
    look.color[3] = tint.a;
    g_materials->apply(material, look);
    if (look.alpha_cutoff <= 0.0f || look.blend != 0 || !look.base_color_texture.valid()) return false;
    out.texture = look.base_color_texture;
    out.cutoff = look.alpha_cutoff;
    out.alpha = look.color[3];
    return true;
}
lsd::editor::DecalAtlas* g_decals = nullptr;  // decal textures for draw_world
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

bool g_lightmaps_changed = true;  // the GPU's lightmap atlas and lightmapped meshes need (re)making
lsd::rhi::TextureHandle g_lightmap_atlas;
std::unordered_map<std::string, std::vector<lsd::i32>> g_lightmap_names;  // object name -> its lightmap objects

// The level's baked lighting from its _BuiltData file (none: the Sky Light's flat ambient).
void load_level_lighting(const char* level_path) {
    const bool found = level_path && level_path[0] && lsd::editor::load_baked_lighting(lsd::editor::baked_lighting_path(level_path).c_str(), g_level_lighting);
    if (!found) g_level_lighting = {};
    lsd::editor::set_baked_lighting(found ? &g_level_lighting : nullptr);
    g_lightmaps_changed = true;
}

// The lightmap atlas on the GPU and each lightmapped object's mesh as "Lightmap:<n>" in the cache.
void upload_lightmaps(lsd::rhi::Device& device, lsd::editor::MeshCache& meshes) {
    g_lightmaps_changed = false;
    device.set_lightmap_atlas({});
    if (g_lightmap_atlas.valid()) device.destroy_texture(g_lightmap_atlas);
    g_lightmap_atlas = {};
    g_lightmap_names.clear();
    const lsd::editor::BakedLighting& l = g_level_lighting;
    if (!l.has_lightmap()) return;
    const void* mips[1] = {l.lightmap.data()};
    g_lightmap_atlas = device.create_texture(l.lightmap_size, l.lightmap_size, 1, lsd::rhi::TextureFormat::RGBA8, mips);
    device.set_lightmap_atlas(g_lightmap_atlas);
    for (lsd::usize k = 0; k < l.objects.size(); ++k) {
        const lsd::editor::LightmapObject& o = l.objects[k];
        lsd::render::MeshData data;
        data.vertices = const_cast<lsd::rhi::MeshVertex*>(o.vertices.data());  // set_generated only reads
        data.indices = const_cast<lsd::u32*>(o.indices.data());
        data.vertex_count = static_cast<lsd::u32>(o.vertices.size());
        data.index_count = static_cast<lsd::u32>(o.indices.size());
        char key[32];
        std::snprintf(key, sizeof key, "Lightmap:%u", static_cast<unsigned>(k));
        meshes.set_generated(key, data);
        g_lightmap_names[o.name].push_back(static_cast<lsd::i32>(k));
    }
}

// The baked copy of this object's mesh to draw (Build Lighting), or null: same name and mesh, where it was baked.
const lsd::editor::CachedMesh* lightmapped_mesh(lsd::editor::MeshCache& meshes, const char* name, const char* mesh, const lsd::Mat4& m) {
    if (g_lightmap_names.empty()) return nullptr;
    const auto it = g_lightmap_names.find(name);
    if (it == g_lightmap_names.end()) return nullptr;
    const lsd::Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    for (const lsd::i32 k : it->second) {
        const lsd::editor::LightmapObject& o = g_level_lighting.objects[static_cast<lsd::usize>(k)];
        const lsd::Vec3 d = at - lsd::Vec3{o.position[0], o.position[1], o.position[2]};
        if (std::strcmp(o.mesh, mesh) != 0 || lsd::dot(d, d) > 1e-4f) continue;
        char key[32];
        std::snprintf(key, sizeof key, "Lightmap:%d", k);
        return meshes.get(key);
    }
    return nullptr;
}

// Skinned shadow casters (within `range` of `light` when range > 0).
void draw_skinned_casters(lsd::rhi::Device& device, const lsd::Mat4& vp, lsd::world::World& world, bool skip_hidden, const lsd::Vec3* light, lsd::f32 range) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::SkeletalMesh>();
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const lsd::world::Entity e = world.owner(type, i);
        if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
        const auto* component = static_cast<const lsd::world::SkeletalMesh*>(world.at(type, i));
        const lsd::editor::SkinnedEntry* model = g_skinned->get(component->mesh);
        const lsd::anim::Pose* pose = g_animations->pose(e);
        if (!model || !pose || !pose->joint_count()) continue;
        const lsd::Mat4 m = lsd::editor::skeletal_matrix(world, e, *component, model);
        if (light && range > 0.0f) {
            const lsd::Vec3 centre = lsd::transform_point(m, (model->model->bounds_min + model->model->bounds_max) * 0.5f);
            const lsd::Vec3 extent = lsd::transform_vector(m, (model->model->bounds_max - model->model->bounds_min) * 0.5f);
            if (lsd::length(centre - *light) > range + lsd::length(extent) * 2.0f) continue;  // generous: the pose may reach out
        }
        const lsd::Mat4 mvp = vp * m;
        const lsd::rhi::BufferHandle vertices = g_animations->vertices(e, model->vertices);
        // Sections with a Masked material cast their cut-out: then each section is its own draw.
        lsd::rhi::ShadowCutout cut[lsd::asset::kMaxMaterialSlots];
        bool masked[lsd::asset::kMaxMaterialSlots] = {}, any = false;
        for (lsd::u32 k = 0; k < model->section_count; ++k) any |= masked[k] = shadow_cutout(lsd::editor::section_material(*component, *model, k), component->color, cut[k]);
        if (!any) {
            device.draw_shadow_caster(vertices, model->indices, model->index_count, mvp.m, pose->joint_rows(), pose->joint_count());
            continue;
        }
        for (lsd::u32 k = 0; k < model->section_count; ++k) {
            device.draw_shadow_caster(vertices, model->sections[k].indices, model->sections[k].index_count, mvp.m, pose->joint_rows(), pose->joint_count(), masked[k] ? &cut[k] : nullptr);
        }
    }
}

// The sun's shadow cascades: every visible mesh into each tile of the atlas, then back to the window.
// Each copy's world matrix as three rows (rhi::MeshDraw::instance_rows); also the full matrices.
void instance_rows(const lsd::world::World& world, lsd::world::Entity e, const lsd::world::InstancedStaticMesh& ism, std::vector<lsd::f32>& rows,
                   std::vector<lsd::Mat4>* matrices = nullptr) {
    const lsd::Mat4 base = world.world_matrix(e);
    rows.resize(static_cast<size_t>(ism.instances.count) * 12);
    if (matrices) matrices->resize(ism.instances.count);
    for (lsd::u32 k = 0; k < ism.instances.count; ++k) {
        const lsd::world::MeshInstance& in = ism.instances[k];
        const lsd::Mat4 m = base * lsd::trs(in.position, in.rotation, in.scale);  // degrees, like Transform
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 4; ++c) rows[k * 12 + static_cast<size_t>(row * 4 + c)] = m.at(row, c);
        }
        if (matrices) (*matrices)[k] = m;
    }
}

// Instanced Static Meshes into a shadow map (one caster per copy).
void draw_instanced_casters(lsd::rhi::Device& device, const lsd::Mat4& vp, lsd::world::World& world, lsd::editor::MeshCache& meshes, bool skip_hidden) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::InstancedStaticMesh>();
    static std::vector<lsd::f32> rows;
    static std::vector<lsd::Mat4> matrices;
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const lsd::world::Entity e = world.owner(type, i);
        const auto* ism = static_cast<const lsd::world::InstancedStaticMesh*>(world.at(type, i));
        if (!ism->cast_shadows || (skip_hidden && world.hidden_in_hierarchy(e))) continue;
        const lsd::editor::CachedMesh* mesh = meshes.get(ism->mesh);
        if (!mesh) continue;
        instance_rows(world, e, *ism, rows, &matrices);
        device.draw_shadow_instances(mesh->gpu.vertices, mesh->gpu.indices, mesh->gpu.index_count, vp.m, rows.data(), ism->instances.count);
    }
}

// Every Static Mesh's world matrix and plain mesh (null: hidden or missing), worked out once a frame
// and shared by the shadow passes and the main pass (each used to recompute them: 5+ times a frame).
struct FrameMeshes {
    std::vector<lsd::Mat4> world;
    std::vector<const lsd::editor::CachedMesh*> mesh;
    void fill(lsd::world::World& w, lsd::editor::MeshCache& meshes, bool skip_hidden) {
        const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
        const lsd::u32 n = w.count(type);
        world.resize(n);
        mesh.resize(n);
        for (lsd::u32 i = 0; i < n; ++i) {
            const lsd::world::Entity e = w.owner(type, i);
            const bool hidden = skip_hidden && w.hidden_in_hierarchy(e);
            mesh[i] = hidden ? nullptr : meshes.get(static_cast<const lsd::world::StaticMesh*>(w.at(type, i))->mesh);
            world[i] = mesh[i] ? w.world_matrix(e) : lsd::Mat4{};
        }
    }
};
FrameMeshes g_frame_meshes;

// Shadow casters of the same mesh and LOD, gathered per cascade / tile and drawn instanced.
struct Caster {
    lsd::rhi::BufferHandle vertices, indices;
    lsd::u32 count;
    lsd::Mat4 world;
    bool masked = false;  // a Masked material: drawn on its own with `cutout`
    lsd::rhi::ShadowCutout cutout;
};

// The clock sprites animate by: game time while playing (so Flipbook events land on the frame you see), else the editor's.
lsd::f32 g_sprite_seconds = 0.0f;
lsd::f32 g_view_distance_scale = 1.0f;  // Engine Scalability > View Distance
const lsd::asset::AssetRegistry* g_registry = nullptr;  // Flipbook assets for the sprites

// Lit Sprites and Tile Maps in a shadow pass: their cut-out shapes (Paper2D's Cast Shadow).
void add_flat_casters(lsd::world::World& world, lsd::editor::MeshCache& meshes, bool skip_hidden, std::vector<Caster>& casters) {
    auto add = [&](lsd::world::Entity e, const lsd::editor::CachedMesh* mesh, const char* texture, lsd::f32 cutoff, lsd::f32 alpha) {
        if (!mesh || (skip_hidden && world.hidden_in_hierarchy(e))) return;
        Caster caster{mesh->gpu.vertices, mesh->gpu.indices, mesh->gpu.index_count, world.world_matrix(e), false, {}};
        if (texture[0] && g_materials) {
            caster.cutout.texture = g_materials->textures().get(texture, true);
            caster.cutout.cutoff = cutoff > 0.0f ? cutoff : 0.5f;  // translucent sprites: their visible part
            caster.cutout.alpha = alpha;
            caster.masked = caster.cutout.texture.valid();
        }
        casters.push_back(caster);
    };
    const lsd::reflect::TypeInfo& sprite_type = lsd::reflect::type_of<lsd::world::Sprite>();
    for (lsd::u32 i = 0; i < world.count(sprite_type); ++i) {
        const auto* sprite = static_cast<const lsd::world::Sprite*>(world.at(sprite_type, i));
        if (!sprite->lit || !sprite->cast_shadows) continue;
        const lsd::u32 cell = lsd::editor::sprite_cell(*sprite, g_sprite_seconds, lsd::editor::find_flipbook(g_registry, sprite->flipbook));
        add(world.owner(sprite_type, i), lsd::editor::sprite_mesh(meshes, *sprite, cell), sprite->texture, sprite->alpha_cutoff, sprite->tint.a);
    }
    const lsd::reflect::TypeInfo& map_type = lsd::reflect::type_of<lsd::world::TileMap>();
    for (lsd::u32 i = 0; i < world.count(map_type); ++i) {
        const auto* map = static_cast<const lsd::world::TileMap*>(world.at(map_type, i));
        if (!map->lit || !map->cast_shadows) continue;
        const lsd::world::Entity e = world.owner(map_type, i);
        add(e, lsd::editor::tile_map_mesh(meshes, *map, e.value), map->tileset, 0.5f, map->tint.a);
    }
}

void draw_casters(lsd::rhi::Device& device, const lsd::Mat4& vp, std::vector<Caster>& casters) {
    std::sort(casters.begin(), casters.end(), [](const Caster& a, const Caster& b) {
        return a.vertices.value != b.vertices.value ? a.vertices.value < b.vertices.value : a.indices.value < b.indices.value;
    });
    static std::vector<lsd::f32> rows;
    for (size_t i = 0; i < casters.size();) {
        if (casters[i].masked) {  // cut-out shadows are not instanced
            const lsd::Mat4 mvp = vp * casters[i].world;
            device.draw_shadow_caster(casters[i].vertices, casters[i].indices, casters[i].count, mvp.m, nullptr, 0, &casters[i].cutout);
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < casters.size() && !casters[end].masked && casters[end].vertices.value == casters[i].vertices.value && casters[end].indices.value == casters[i].indices.value) ++end;
        const Caster& c = casters[i];
        if (end - i == 1) {
            const lsd::Mat4 mvp = vp * c.world;
            device.draw_shadow_caster(c.vertices, c.indices, c.count, mvp.m);
        } else {
            rows.clear();
            for (size_t k = i; k < end; ++k) {
                for (int r = 0; r < 3; ++r) {
                    for (int col = 0; col < 4; ++col) rows.push_back(casters[k].world.at(r, col));
                }
            }
            device.draw_shadow_instances(c.vertices, c.indices, c.count, vp.m, rows.data(), static_cast<lsd::u32>(end - i));
        }
        i = end;
    }
    casters.clear();
}

// This frame's Cull Distance Volumes (filled by draw_world before any pass): the inverse of each
// box's matrix and its rules.
struct CullVolume {
    lsd::Mat4 to_local;
    const lsd::world::CullDistanceVolume* rules;
};
std::vector<CullVolume> g_cull_volumes;

void gather_cull_volumes(const lsd::world::World& world) {
    g_cull_volumes.clear();
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::CullDistanceVolume>();
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const auto* v = static_cast<const lsd::world::CullDistanceVolume*>(world.at(type, i));
        if (v->enabled && v->cull_distances.count) g_cull_volumes.push_back({lsd::affine_inverse(world.world_matrix(world.owner(type, i))), v});
    }
}

// The distance beyond which an object `size` across at `centre` is culled by the volumes it is in; 0 = none.
lsd::f32 volume_cull_distance(lsd::Vec3 centre, lsd::f32 size) {
    lsd::f32 result = 0.0f;
    for (const CullVolume& v : g_cull_volumes) {
        const lsd::Vec3 p = lsd::transform_point(v.to_local, centre);
        if (p.x < -0.5f || p.x > 0.5f || p.y < -0.5f || p.y > 0.5f || p.z < 0.0f || p.z > 1.0f) continue;  // the unit volume box
        const lsd::world::CullDistance* best = nullptr;  // the smallest rule that fits, else the biggest
        for (const lsd::world::CullDistance& r : v.rules->cull_distances) {
            const bool fits = r.size >= size, best_fits = best && best->size >= size;
            if (!best || (fits && (!best_fits || r.size < best->size)) || (!fits && !best_fits && r.size > best->size)) best = &r;
        }
        if (best && best->distance > 0.0f && (result == 0.0f || best->distance < result)) result = best->distance;
    }
    return result;
}

// What to draw of a static mesh at world matrix `m` seen from `eye`: false past its Max Draw Distance
// or its Cull Distance Volume's distance (not drawn at all), else the index buffer of the LOD its
// on-screen size picks (r.ForceLOD forces one).
bool pick_static_lod(const lsd::editor::CachedMesh& mesh, const lsd::world::StaticMesh& component, const lsd::Mat4& m, lsd::Vec3 eye, lsd::i32 force_lod,
                     lsd::rhi::BufferHandle& indices, lsd::u32& count) {
    const lsd::Vec3 centre = lsd::transform_point(m, (mesh.bounds_min + mesh.bounds_max) * 0.5f);
    // Shorter (or longer) view distance: everything is treated as further away (or nearer).
    const lsd::f32 distance = std::fmax(lsd::length(centre - eye), 0.001f) / g_view_distance_scale;
    if (component.max_draw_distance > 0.0f && distance > component.max_draw_distance) return false;
    const lsd::Vec3 half = (mesh.bounds_max - mesh.bounds_min) * 0.5f;
    const lsd::f32 scale = std::fmax(lsd::length(lsd::Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}),
                                     std::fmax(lsd::length(lsd::Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}), lsd::length(lsd::Vec3{m.at(0, 2), m.at(1, 2), m.at(2, 2)})));
    if (!g_cull_volumes.empty()) {
        const lsd::f32 cull = volume_cull_distance(centre, lsd::length(half) * scale * 2.0f);
        if (cull > 0.0f && distance > cull) return false;
    }
    indices = mesh.gpu.indices;
    count = mesh.gpu.index_count;
    if (mesh.lod_count <= 1) return true;
    const lsd::u32 lod = mesh.pick_lod(lsd::length(half) * scale / distance, force_lod);
    if (lod > 0) {
        indices = mesh.lod_indices[lod];
        count = mesh.lod_index_count[lod];
    }
    return true;
}

// Frustum culling: false when the box (local bounds under `mvp`) is wholly outside one clip plane.
// `sides_only`: just the four side planes (shadow casters in front of a pancaked cascade still cast).
bool in_view(const lsd::Mat4& mvp, lsd::Vec3 lo, lsd::Vec3 hi, bool sides_only = false) {
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
    for (int k = 0; k < (sides_only ? 4 : 6); ++k) {
        if (outside[k] == 8) return false;
    }
    return true;
}

// Shadow casters use the LOD the camera sees (a far tree casts its simple shape), not the light's view.
void draw_shadows(lsd::rhi::Device& device, const lsd::rhi::LightSetup& lights, lsd::rhi::TextureHandle atlas, lsd::world::World& world,
                  lsd::editor::MeshCache& meshes, bool skip_hidden, lsd::Vec3 eye, lsd::i32 force_lod) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    device.begin_shadow_pass(atlas);
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::shadow_atlas_size());
    static std::vector<Caster> casters;
    for (lsd::u32 c = 0; c < static_cast<lsd::u32>(lights.shadow_info[0]); ++c) {
        device.set_viewport(lights.shadow_tiles[c][0] * size, lights.shadow_tiles[c][1] * size, lights.shadow_tiles[c][2] * size, lights.shadow_tiles[c][2] * size);
        lsd::Mat4 vp;
        std::memcpy(vp.m, lights.shadow_matrices[c], sizeof vp.m);
        for (lsd::u32 i = 0; i < world.count(type) && i < g_frame_meshes.mesh.size(); ++i) {
            const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
            const lsd::editor::CachedMesh* mesh = g_frame_meshes.mesh[i];  // null: hidden or missing
            if (!mesh) continue;
            const lsd::Mat4& m = g_frame_meshes.world[i];
            lsd::rhi::BufferHandle indices;
            lsd::u32 count = 0;
            const lsd::Mat4 mvp = vp * m;
            if (!in_view(mvp, mesh->bounds_min, mesh->bounds_max, true)) continue;  // beside this cascade: its shadow falls elsewhere
            if (!pick_static_lod(*mesh, *component, m, eye, force_lod, indices, count)) continue;
            Caster caster{mesh->gpu.vertices, indices, count, m, false, {}};
            caster.masked = shadow_cutout(component->material, component->color, caster.cutout);
            casters.push_back(caster);
        }
        add_flat_casters(world, meshes, skip_hidden, casters);
        draw_casters(device, vp, casters);
        draw_skinned_casters(device, vp, world, skip_hidden, nullptr, 0.0f);
        draw_instanced_casters(device, vp, world, meshes, skip_hidden);
    }
    device.end_shadow_pass();
}

// Spot / point light shadow tiles: the meshes that reach into each light's range.
void draw_local_shadows(lsd::rhi::Device& device, const lsd::rhi::LightSetup& lights, lsd::rhi::TextureHandle atlas, lsd::world::World& world,
                        lsd::editor::MeshCache& meshes, bool skip_hidden, lsd::Vec3 eye, lsd::i32 force_lod) {
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    device.begin_shadow_pass(atlas);
    const lsd::f32 size = static_cast<lsd::f32>(lsd::editor::shadow_atlas_size());
    static std::vector<Caster> casters;
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
            for (lsd::u32 i = 0; i < world.count(type) && i < g_frame_meshes.mesh.size(); ++i) {
                const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
                const lsd::editor::CachedMesh* mesh = g_frame_meshes.mesh[i];  // null: hidden or missing
                if (!mesh) continue;
                const lsd::Mat4& m = g_frame_meshes.world[i];
                // Skip meshes whose bounding sphere does not reach the light.
                const lsd::Vec3 centre = lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f);
                const lsd::Vec3 extent = lsd::transform_vector(m, (mesh->bounds_max - mesh->bounds_min) * 0.5f);
                if (lsd::length(centre - at) > light.range + lsd::length(extent) * 1.8f) continue;
                const lsd::Mat4 mvp = vp * m;
                if (!in_view(mvp, mesh->bounds_min, mesh->bounds_max, true)) continue;  // outside this tile's cone / cube face
                lsd::rhi::BufferHandle indices;
                lsd::u32 count = 0;
                if (!pick_static_lod(*mesh, *component, m, eye, force_lod, indices, count)) continue;
                Caster caster{mesh->gpu.vertices, indices, count, m, false, {}};
                caster.masked = shadow_cutout(component->material, component->color, caster.cutout);
                casters.push_back(caster);
            }
            add_flat_casters(world, meshes, skip_hidden, casters);
            draw_casters(device, vp, casters);
            draw_skinned_casters(device, vp, world, skip_hidden, &at, light.range);
            draw_instanced_casters(device, vp, world, meshes, skip_hidden);
        }
    }
    device.end_shadow_pass();
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

using BlendQueue = std::vector<std::pair<lsd::f32, lsd::rhi::MeshDraw>>;

// The Overdraw view mode: every mesh adds a little light, without a depth test. Otherwise translucent
// and additive draws wait (with their distance) until the opaque ones are done; true when queued.
// Automatic instancing: opaque Static Meshes with the same mesh, LOD and look are drawn as one
// instanced call (Unreal's auto-instancing). Material Graphs and translucency are drawn one by one
// as before; mirrored objects (negative scale) too, since their triangles face the other way.
struct BatchItem {
    lsd::u64 key;
    lsd::rhi::MeshDraw draw;
    lsd::Mat4 world;
};

bool can_batch(const lsd::rhi::MeshDraw& d, const lsd::Mat4& m) {
    const lsd::Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    const bool mirrored = lsd::dot(lsd::cross(c0, c1), c2) < 0.0f;
    return !d.material_shader.valid() && d.blend == 0 && !d.wireframe && d.debug_view != 4 && !mirrored;
}

lsd::u64 batch_key(const lsd::rhi::MeshDraw& d) {  // everything but the transform
    lsd::u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const lsd::u8*>(p)[i]) * 1099511628211ull;
    };
    mix(&d.vertices.value, 4), mix(&d.indices.value, 4), mix(&d.index_count, 4), mix(d.color, sizeof d.color);
    mix(&d.metallic, 4), mix(&d.roughness, 4), mix(d.emissive, sizeof d.emissive), mix(&d.base_color_texture.value, 4);
    mix(&d.normal_texture.value, 4), mix(&d.orm_texture.value, 4), mix(&d.alpha_cutoff, 4);
    const lsd::u32 flags = (d.unlit ? 1u : 0u) | (d.pixel_art ? 2u : 0u) | (d.debug_view << 2);
    mix(&flags, 4);
    return h;
}

void draw_batches(lsd::rhi::Device& device, std::vector<BatchItem>& items, const lsd::Mat4& view_proj, EditorState& stats) {
    std::sort(items.begin(), items.end(), [](const BatchItem& a, const BatchItem& b) { return a.key < b.key; });
    static std::vector<lsd::f32> rows;
    for (size_t i = 0; i < items.size();) {
        size_t end = i + 1;
        while (end < items.size() && items[end].key == items[i].key) ++end;
        lsd::rhi::MeshDraw draw = items[i].draw;
        if (end - i == 1) {
            device.draw_mesh(draw);  // alone: its own mvp / normals, as before
        } else {
            rows.clear();
            for (size_t k = i; k < end; ++k) {
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) rows.push_back(items[k].world.at(r, c));
                }
            }
            std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
            draw.instance_rows = rows.data();
            draw.instance_count = static_cast<lsd::u32>(end - i);
            device.draw_mesh(draw);
        }
        ++stats.stat_draws;
        stats.stat_triangles += draw.index_count / 3 * static_cast<lsd::u32>(end - i);
        i = end;
    }
    items.clear();
}

bool blend_view(const EditorState& stats, lsd::rhi::MeshDraw& draw, lsd::Vec3 centre, lsd::Vec3 eye, BlendQueue& queue) {
    if ((!stats.playing || stats.ejected) && stats.view_mode == lsd::editor::ViewMode::Overdraw) {
        draw.blend = 3;
        draw.debug_view = 4;
        return false;
    }
    if (draw.wireframe || draw.blend == 0) return false;
    queue.emplace_back(lsd::length(centre - eye), draw);
    return true;
}

void draw_world(lsd::rhi::Device& device, const lsd::Mat4& view_proj, const lsd::editor::LightView& light_view, const lsd::f32 viewport[4],
                lsd::rhi::TextureHandle shadow_atlas, lsd::rhi::TextureHandle local_atlas, lsd::world::World& world, lsd::editor::MeshCache& meshes, lsd::editor::MaterialCache& materials,
                EditorState& stats, lsd::rhi::TextureHandle scene_target = {}, const lsd::f32* scene_clear = nullptr, bool hdr = true) {
    LSD_PROFILE_SCOPE("draw_world");
    static BlendQueue translucent;  // reused each call: no allocation once it has grown
    translucent.clear();
    static std::vector<BatchItem> batch;  // opaque meshes waiting to be drawn instanced
    g_outline.clear();  // this view's selected objects, gathered below
    const lsd::reflect::TypeInfo& type = lsd::reflect::type_of<lsd::world::StaticMesh>();
    const lsd::Vec3 eye = light_view.eye;
    const bool skip_hidden = !stats.playing || stats.ejected;
    lsd::rhi::LightSetup lights = lsd::editor::world_lights(world, light_view, skip_hidden);
    if (g_decals) g_decals->apply(world, skip_hidden, light_view.eye, lights);
    const bool sun_shadows = lights.shadow_info[1] >= 0.0f && shadow_atlas.valid() && !stats.view_mode_unlit_or_wire();
    const bool local_shadows = lights.local_tiles[0][3] > 0.0f && local_atlas.valid() && !stats.view_mode_unlit_or_wire();
    gather_cull_volumes(world);
    g_frame_meshes.fill(world, meshes, skip_hidden);  // matrices and meshes once, for every pass below
    if (sun_shadows) draw_shadows(device, lights, shadow_atlas, world, meshes, skip_hidden, eye, stats.force_lod);
    if (std::exchange(g_gpu_mark_shadows, false)) device.gpu_timestamp(1);  // the first view's shadows: the frame's main cost
    if (local_shadows) draw_local_shadows(device, lights, local_atlas, world, meshes, skip_hidden, eye, stats.force_lod);
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
    stats.stat_occluded = 0;
    // Occlusion culling: the nearest big opaque meshes in view go into a small CPU depth buffer (their
    // real triangles, up to 4096 each), then every static mesh's box is tested against it below.
    static lsd::render::OcclusionBuffer occlusion;  // 128 KB: kept, not on the stack
    occlusion.clear();
    if (g_occlusion_culling && !stats.view_mode_unlit_or_wire()) {
        struct Candidate {
            lsd::f32 size;
            lsd::u32 index;
        };
        static std::vector<Candidate> candidates;
        candidates.clear();
        for (lsd::u32 i = 0; i < world.count(type); ++i) {
            const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
            const lsd::world::Entity e = world.owner(type, i);
            if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
            const lsd::editor::CachedMesh* mesh = meshes.get(component->mesh);
            if (!mesh) continue;
            const lsd::Mat4 m = world.world_matrix(e);
            if (!in_view(view_proj * m, mesh->bounds_min, mesh->bounds_max)) continue;
            const lsd::Vec3 centre = lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f);
            const lsd::f32 radius = lsd::length(lsd::transform_vector(m, (mesh->bounds_max - mesh->bounds_min) * 0.5f));
            const lsd::f32 size = radius / std::max(lsd::length(centre - eye), 0.1f);  // how big it looks
            if (size > 0.1f) candidates.push_back({size, i});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.size > b.size; });
        for (lsd::usize k = 0; k < candidates.size() && k < 16; ++k) {
            const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, candidates[k].index));
            lsd::rhi::MeshDraw look{};
            to_rgba(component->color, look.color);
            materials.apply(component->material, look);
            if (look.blend != 0 || look.alpha_cutoff > 0.0f || look.color[3] < 0.999f) continue;  // see-through: hides nothing
            const lsd::editor::MeshCache::Triangles* tris = meshes.triangles(component->mesh);
            if (!tris || tris->indices.empty() || tris->indices.size() > 3 * 4096) continue;  // heavy meshes are not worth it on the CPU
            const lsd::world::Entity e = world.owner(type, candidates[k].index);
            occlusion.add_occluder(view_proj * world.world_matrix(e), tris->points.data(), tris->indices.data(), static_cast<lsd::u32>(tris->indices.size()));
        }
    }
    for (lsd::u32 i = 0; i < world.count(type); ++i) {
        const auto* component = static_cast<const lsd::world::StaticMesh*>(world.at(type, i));
        const lsd::world::Entity e = world.owner(type, i);
        if ((!stats.playing || stats.ejected) && world.hidden_in_hierarchy(e)) continue;  // hidden in the editor, not in the game
        const lsd::Mat4 m = i < g_frame_meshes.world.size() && g_frame_meshes.mesh[i] ? g_frame_meshes.world[i] : world.world_matrix(e);
        const lsd::Vec3 world_scale{lsd::length(lsd::Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}), lsd::length(lsd::Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}),
                                    lsd::length(lsd::Vec3{m.at(0, 2), m.at(1, 2), m.at(2, 2)})};
        const lsd::editor::CachedMesh* mesh = lightmapped_mesh(meshes, world.name(e), component->mesh, m);
        if (!mesh) mesh = component->box_uv ? meshes.get_box_uv(component->mesh, world_scale) : meshes.get(component->mesh);
        if (!mesh) continue;
        const lsd::Mat4 mvp = view_proj * m;
        if (!in_view(mvp, mesh->bounds_min, mesh->bounds_max)) {
            ++stats.stat_culled;
            continue;
        }
        if (!occlusion.box_visible(mvp, mesh->bounds_min, mesh->bounds_max)) {  // behind the occluders
            ++stats.stat_culled;
            ++stats.stat_occluded;
            continue;
        }
        lsd::rhi::MeshDraw draw{};
        if (!pick_static_lod(*mesh, *component, m, eye, stats.force_lod, draw.indices, draw.index_count)) {  // distant meshes draw a simpler LOD
            ++stats.stat_culled;
            continue;
        }
        const lsd::Mat4 inv = lsd::affine_inverse(m);  // normals use the inverse transpose (correct under non-uniform scale)
        draw.vertices = mesh->gpu.vertices;
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
        if (std::strncmp(component->mesh, "Landscape:", 10) == 0) {  // painted layers, blended
            if (const lsd::rhi::TextureHandle t = lsd::editor::landscape_texture(component->mesh); t.valid()) {
                draw.base_color_texture = t;
                draw.color[0] = draw.color[1] = draw.color[2] = 1.0f;  // the layers carry their own tints
            }
        }
        const bool editor_view = !stats.playing || stats.ejected;
        draw.unlit = editor_view && (stats.view_mode == lsd::editor::ViewMode::Unlit || stats.view_mode == lsd::editor::ViewMode::Collision);
        draw.debug_view = !editor_view ? 0u : stats.view_mode == lsd::editor::ViewMode::Normals ? 2u : stats.view_mode == lsd::editor::ViewMode::LightingOnly ? 3u : 0u;
        draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
        if ((!stats.playing || stats.ejected) && stats.is_selected(e)) g_outline.push_back(draw);  // outlined after the post pass
        if (blend_view(stats, draw, lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f), eye, translucent)) continue;
        if (can_batch(draw, m)) {  // drawn below with the others like it
            batch.push_back({batch_key(draw), draw, m});
            continue;
        }
        device.draw_mesh(draw);
        ++stats.stat_draws;
        stats.stat_triangles += draw.index_count / 3;
    }
    draw_batches(device, batch, view_proj, stats);
    // 2D (Paper2D): Sprites and Tile Maps, textured quads in the X-Z plane; unlit unless they ask
    // (lit ones cast shadows: add_flat_casters). Translucent sprites wait with the translucent meshes,
    // ordered by Sort Order first, then far to near.
    {
        const bool editor_view = !stats.playing || stats.ejected;
        auto draw_flat = [&](lsd::world::Entity e, const lsd::editor::CachedMesh* mesh, const char* texture, const lsd::LinearColor& tint, bool pixel_art, bool lit, lsd::f32 cutoff,
                             bool blended = false, lsd::i32 sort_order = 0) {
            if (!mesh || (editor_view && world.hidden_in_hierarchy(e))) return;
            const lsd::Mat4 m = world.world_matrix(e);
            const lsd::Mat4 mvp = view_proj * m;
            if (!in_view(mvp, mesh->bounds_min, mesh->bounds_max)) {
                ++stats.stat_culled;
                return;
            }
            lsd::rhi::MeshDraw draw{};
            draw.vertices = mesh->gpu.vertices;
            draw.indices = mesh->gpu.indices;
            draw.index_count = mesh->gpu.index_count;
            std::memcpy(draw.mvp, mvp.m, sizeof draw.mvp);
            const lsd::Mat4 inv = lsd::affine_inverse(m);
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
            to_rgba(tint, draw.color);
            draw.roughness = 1.0f;
            if (texture[0]) draw.base_color_texture = materials.textures().get(texture, true);
            draw.pixel_art = pixel_art;
            draw.alpha_cutoff = cutoff;
            draw.unlit = !lit || (editor_view && (stats.view_mode == lsd::editor::ViewMode::Unlit || stats.view_mode == lsd::editor::ViewMode::Collision));
            draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
            if (editor_view && stats.is_selected(e)) g_outline.push_back(draw);
            if (blended && !draw.wireframe) {
                draw.blend = 1;
                draw.alpha_cutoff = 0.0f;
                const lsd::Vec3 centre = lsd::transform_point(m, (mesh->bounds_min + mesh->bounds_max) * 0.5f);
                translucent.emplace_back(lsd::length(centre - eye) - static_cast<lsd::f32>(sort_order) * 1.0e5f, draw);  // drawn largest first
                return;
            }
            device.draw_mesh(draw);
            ++stats.stat_draws;
            stats.stat_triangles += draw.index_count / 3;
        };
        const lsd::reflect::TypeInfo& sprite_type = lsd::reflect::type_of<lsd::world::Sprite>();
        for (lsd::u32 i = 0; i < world.count(sprite_type); ++i) {
            const auto* sprite = static_cast<const lsd::world::Sprite*>(world.at(sprite_type, i));
            const lsd::u32 cell = lsd::editor::sprite_cell(*sprite, g_sprite_seconds, lsd::editor::find_flipbook(stats.registry, sprite->flipbook));
            const lsd::editor::CachedMesh* mesh = lsd::editor::sprite_mesh(meshes, *sprite, cell);
            draw_flat(world.owner(sprite_type, i), mesh, sprite->texture, sprite->tint, sprite->pixel_art, sprite->lit, sprite->alpha_cutoff, sprite->translucent, sprite->sort_order);
        }
        const lsd::reflect::TypeInfo& map_type = lsd::reflect::type_of<lsd::world::TileMap>();
        for (lsd::u32 i = 0; i < world.count(map_type); ++i) {
            const auto* map = static_cast<const lsd::world::TileMap*>(world.at(map_type, i));
            const lsd::world::Entity e = world.owner(map_type, i);
            draw_flat(e, lsd::editor::tile_map_mesh(meshes, *map, e.value), map->tileset, map->tint, map->pixel_art, map->lit, 0.5f);
        }
    }
    // Instanced Static Meshes: every copy in one draw (a Material Graph draws them one by one).
    {
        const lsd::reflect::TypeInfo& ism_type = lsd::reflect::type_of<lsd::world::InstancedStaticMesh>();
        static std::vector<lsd::f32> rows;
        static std::vector<lsd::Mat4> matrices;
        for (lsd::u32 i = 0; i < world.count(ism_type); ++i) {
            const auto* ism = static_cast<const lsd::world::InstancedStaticMesh*>(world.at(ism_type, i));
            const lsd::world::Entity e = world.owner(ism_type, i);
            if (((!stats.playing || stats.ejected) && world.hidden_in_hierarchy(e)) || !ism->instances.count) continue;
            const lsd::editor::CachedMesh* mesh = meshes.get(ism->mesh);
            if (!mesh) continue;
            instance_rows(world, e, *ism, rows, &matrices);
            lsd::rhi::MeshDraw draw{};
            draw.vertices = mesh->gpu.vertices;
            draw.indices = mesh->gpu.indices;
            draw.index_count = mesh->gpu.index_count;
            draw.camera_position[0] = eye.x;
            draw.camera_position[1] = eye.y;
            draw.camera_position[2] = eye.z;
            to_rgba(ism->color, draw.color);
            draw.time = static_cast<lsd::f32>(SDL_GetTicks()) * 0.001f;
            materials.apply(ism->material, draw);
            const bool editor_view = !stats.playing || stats.ejected;
            draw.unlit = editor_view && (stats.view_mode == lsd::editor::ViewMode::Unlit || stats.view_mode == lsd::editor::ViewMode::Collision);
        draw.debug_view = !editor_view ? 0u : stats.view_mode == lsd::editor::ViewMode::Normals ? 2u : stats.view_mode == lsd::editor::ViewMode::LightingOnly ? 3u : 0u;
            draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
            if (editor_view && stats.is_selected(e)) {
                draw.color[0] = 1.0f;
                draw.color[1] = 0.85f;
                draw.color[2] = 0.25f;
            }
            if (stats.view_mode == lsd::editor::ViewMode::Overdraw && editor_view) {
                draw.blend = 3;
                draw.debug_view = 4;
            }
            if (!draw.material_shader.valid() || draw.wireframe) {
                std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
                draw.instance_rows = rows.data();
                draw.instance_count = ism->instances.count;
                // Many copies (foliage, crowds of props): the GPU drops those outside the view.
                draw.gpu_cull = g_gpu_culling && ism->instances.count >= 32 && device.supports_gpu_culling();
                std::memcpy(draw.local_bounds_min, &mesh->bounds_min, sizeof draw.local_bounds_min);
                std::memcpy(draw.local_bounds_max, &mesh->bounds_max, sizeof draw.local_bounds_max);
                device.draw_mesh(draw);
                ++stats.stat_draws;
            } else {
                for (const lsd::Mat4& m : matrices) {  // Material Graphs have no instanced variant
                    const lsd::Mat4 mvp = view_proj * m;
                    const lsd::Mat4 inv = lsd::affine_inverse(m);
                    std::memcpy(draw.mvp, mvp.m, sizeof draw.mvp);
                    for (int c = 0; c < 3; ++c) {
                        for (int r = 0; r < 3; ++r) draw.normal_matrix[c * 4 + r] = inv.at(c, r);
                        draw.normal_matrix[c * 4 + 3] = 0.0f;
                    }
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 4; ++c) draw.world[r * 4 + c] = m.at(r, c);
                    }
                    device.draw_mesh(draw);
                    ++stats.stat_draws;
                }
            }
            stats.stat_triangles += mesh->gpu.index_count / 3 * ism->instances.count;
        }
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
        const lsd::Mat4 m = lsd::editor::skeletal_matrix(world, e, *component, model);
        const lsd::Mat4 mvp = view_proj * m;
        // Bind-pose bounds grown by half their size: animation moves limbs out of the rest pose.
        const lsd::Vec3 grow = (model->model->bounds_max - model->model->bounds_min) * 0.5f;
        if (!in_view(mvp, model->model->bounds_min - grow, model->model->bounds_max + grow)) {
            ++stats.stat_culled;
            continue;
        }
        const lsd::Mat4 inv = lsd::affine_inverse(m);
        lsd::rhi::MeshDraw draw{};
        draw.vertices = g_animations->vertices(e, model->vertices);
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
        draw.time = static_cast<lsd::f32>(SDL_GetTicks()) * 0.001f;
        const bool editor_view = !stats.playing || stats.ejected;
        draw.unlit = editor_view && (stats.view_mode == lsd::editor::ViewMode::Unlit || stats.view_mode == lsd::editor::ViewMode::Collision);
        draw.debug_view = !editor_view ? 0u : stats.view_mode == lsd::editor::ViewMode::Normals ? 2u : stats.view_mode == lsd::editor::ViewMode::LightingOnly ? 3u : 0u;
        draw.wireframe = editor_view && stats.view_mode == lsd::editor::ViewMode::Wireframe;
        // One draw per material slot (Unreal's mesh sections), each with its slot's material.
        for (lsd::u32 s = 0; s < model->section_count; ++s) {
            lsd::rhi::MeshDraw section = draw;
            section.indices = model->sections[s].indices;
            section.index_count = model->sections[s].index_count;
            const char* material = lsd::editor::section_material(*component, *model, s);
            if (material[0]) section.color[0] = section.color[1] = section.color[2] = section.color[3] = 1.0f;  // the material's own colours
            else to_rgba(component->color, section.color);
            materials.apply(material, section);
            if (editor_view && stats.is_selected(e)) {
                section.color[0] = 1.0f;
                section.color[1] = 0.85f;
                section.color[2] = 0.25f;
            }
            if (blend_view(stats, section, lsd::Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)}, eye, translucent)) continue;
            device.draw_mesh(section);
            ++stats.stat_draws;
            stats.stat_triangles += section.index_count / 3;
        }
    }
    // Translucent and additive meshes last, far to near, over everything opaque (they do not write depth).
    std::sort(translucent.begin(), translucent.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [distance, draw] : translucent) {
        device.draw_mesh(draw);
        ++stats.stat_draws;
        stats.stat_triangles += draw.index_count / 3;
    }
}

// Console commands and variables (Output Log tab), like Unreal's exec commands and CVars.
void register_console(EditorState& s) {
    namespace console = lsd::console;
    console::add_variable("r.Grid", &s.show_grid, "Draw the floor grid");
    console::add_variable("show.Navigation", &s.show_navigation, "Draw the navmesh (P)");
    console::add_variable("show.Bounds", &s.show_bounds, "Draw each mesh's bounding box");
    console::add_variable("show.Collision", &s.show_collision, "Draw what blocks in Play (meshes with collision, Blocking Volumes, rigid bodies)");
    console::add_variable("r.ForceLOD", &s.force_lod, "Draw every mesh at this LOD (-1: pick by distance)");
    console::add_variable("r.OcclusionCulling", &g_occlusion_culling, "Static meshes hidden behind the nearest big opaque meshes are not drawn (a small CPU depth buffer)");
    console::add_variable("r.GPUCulling", &g_gpu_culling, "Instanced Static Meshes (32+ copies): the GPU drops the copies outside the view (compute + indirect draws)");
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
        else if (std::strcmp(args, "dump") == 0) {  // the numbers as text (remote console, the MCP bridge, tests)
            char line[200];
            std::snprintf(line, sizeof line, "game %.2f ms, render %.2f ms, ui %.2f ms, gpu wait %.2f ms, draws %u, triangles %u, culled %u (occluded %u)", st.stat_game_ms,
                          st.stat_render_ms, st.stat_ui_ms, st.stat_wait_ms, st.stat_draws, st.stat_triangles, st.stat_culled, st.stat_occluded);
            lsd::editor::remote::print(line);
            LSD_LOG_INFO("stat", "%s", line);
            std::snprintf(line, sizeof line, "ui: outliner %.2f, details %.2f, content %.2f, viewports %.2f, rest %.2f ms", st.stat_panel_ms[0], st.stat_panel_ms[1],
                          st.stat_panel_ms[2], st.stat_panel_ms[3], st.stat_panel_ms[4]);
            lsd::editor::remote::print(line);
            if (st.stat_gpu_valid) {  // sections between the marks (a mark not set that frame: its section is folded into the next)
                const lsd::f64* t = st.stat_gpu_ms;
                auto span = [t](int from, int to) { return t[to] >= 0.0 && t[from] >= 0.0 ? t[to] - t[from] : 0.0; };
                const int scene_from = t[1] >= 0.0 ? 1 : 0, post_from = t[2] >= 0.0 ? 2 : scene_from;
                std::snprintf(line, sizeof line, "gpu: shadows %.2f, scene %.2f, post %.2f, ui %.2f, total %.2f ms", span(0, 1), span(scene_from, 2), span(post_from, 3),
                              span(t[3] >= 0.0 ? 3 : post_from, 4), t[4]);
            } else {
                std::snprintf(line, sizeof line, "gpu: no timings from this graphics backend yet");
            }
            lsd::editor::remote::print(line);
        } else LSD_LOG_WARN("console", "stat: try 'stat fps', 'stat audio' or 'stat dump'");
    }, &s, "stat fps | stat audio | stat dump: toggle the fps / draw call or the active sounds overlay; dump prints the frame's numbers");
    console::add_command("camera", [](void* user, const char* args) {
        auto& cam = static_cast<EditorState*>(user)->camera;
        float x, y, z, yaw = 0.0f, pitch = 0.0f;
        if (std::sscanf(args, "%f %f %f %f %f", &x, &y, &z, &yaw, &pitch) < 3) {
            lsd::editor::remote::print("camera <x> <y> <z> [yaw pitch]: degrees, yaw 0 looks along +X, pitch < 0 looks down");
            return;
        }
        cam.position = {x, y, z};
        cam.yaw = yaw * 0.0174533f;
        cam.pitch = pitch * 0.0174533f;
    }, &s, "camera <x> <y> <z> [yaw pitch]: put the viewport camera there (degrees)");
    console::add_command("view", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        static const char* const names[] = {"perspective", "top", "front", "right"};
        for (int v = 0; v < 4; ++v) {
            if (std::strcmp(args, names[v]) == 0) return lsd::editor::set_view(st, static_cast<lsd::editor::ViewType>(v));
        }
        LSD_LOG_WARN("console", "view: perspective, top, front or right");
    }, &s, "view perspective|top|front|right");
    console::add_command("layout", [](void* user, const char* args) {
        const bool two = std::strcmp(args, "2") == 0 || std::strcmp(args, "1x2") == 0;
        lsd::editor::set_layout(*static_cast<EditorState*>(user), two ? 2u : std::strcmp(args, "4") == 0 || std::strcmp(args, "2x2") == 0 ? 4u : 1u);
    }, &s, "layout 1|2|4: one viewport, perspective + top side by side, or perspective + top + front + right");
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
    console::add_command("memreport", [](void*, const char*) {  // every allocator to the log, biggest first
        lsd::AllocatorStats stats[128];
        const lsd::u32 n = lsd::allocator_stats(stats, 128);
        std::sort(stats, stats + n, [](const lsd::AllocatorStats& a, const lsd::AllocatorStats& b) { return a.bytes > b.bytes; });
        lsd::u64 process = 0, peak = 0;
        lsd::process_memory(process, peak);
        LSD_LOG_INFO("memory", "Process: %.1f MiB (peak %.1f MiB); %u allocators:", static_cast<double>(process) / 1048576.0, static_cast<double>(peak) / 1048576.0, n);
        for (lsd::u32 i = 0; i < n; ++i) {
            LSD_LOG_INFO("memory", "  %-24s %10.1f KiB in use, %10.1f KiB %s, %zu blocks", stats[i].name, static_cast<double>(stats[i].bytes) / 1024.0,
                         static_cast<double>(stats[i].peak) / 1024.0, stats[i].arena ? "capacity" : "peak", stats[i].allocations);
        }
    }, &s, "Every allocator's memory to the Output Log (Window > Memory shows it live)");
    console::add_command("BuildLighting", [](void* user, const char*) { static_cast<EditorState*>(user)->build_lighting_requested = true; }, &s,
                         "Build > Build Lighting: bake the level's light probes");
    console::add_command("viewmode", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        static const char* const names[] = {"lit", "unlit", "wireframe", "normals", "lightingonly", "collision", "overdraw"};
        for (int m = 0; m < 7; ++m) {
            if (std::strcmp(args, names[m]) == 0) {
                st.view_mode = static_cast<lsd::editor::ViewMode>(m);
                return;
            }
        }
        LSD_LOG_WARN("console", "viewmode: lit, unlit, wireframe, normals, lightingonly, collision or overdraw");
    }, &s, "viewmode lit|unlit|wireframe|normals|lightingonly|collision|overdraw");
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
    console::add_command("debug.Crash", [](void*, const char* args) {  // tests the crash reporter
        if (std::strcmp(args, "abort") == 0) std::abort();
        if (std::strcmp(args, "terminate") == 0) std::terminate();
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }, &s, "Crash on purpose (writes a crash report to Saved/Crashes): debug.Crash [abort | terminate]");
    console::add_command("debug.Break", [](void*, const char* args) {  // a Lua breakpoint from the console: debug.Break Content/x.lua 12
        char file[260];
        int line = 0;
        if (std::sscanf(args, "%259s %d", file, &line) == 2) lsd::editor::debugger::toggle(file, line);
        else LSD_LOG_WARN("console", "debug.Break <file> <line>");
    }, &s, "Toggle a Lua breakpoint (Tools > Lua Debugger shows them)");
    console::add_command("mesh.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Mesh, nullptr, &hit, 1) == 1) lsd::editor::open_mesh_editor(st, st.registry->get(hit));
        else LSD_LOG_WARN("editor", "mesh.Open: no mesh named like '%s'", args);
    }, &s, "Open a mesh in the Mesh Editor");
    console::add_command("prefab.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::Prefab, nullptr, &hit, 1) == 1) lsd::editor::open_prefab_editor(st, st.registry->get(hit));
        else LSD_LOG_WARN("editor", "prefab.Open: no prefab named like '%s'", args);
    }, &s, "Open a prefab in the Prefab editor");
    console::add_command("timeline.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hits[16];
        const lsd::usize n = st.registry->search(args, lsd::asset::AssetType::Data, nullptr, hits, 16);
        for (lsd::usize i = 0; i < n; ++i) {
            const lsd::asset::AssetRecord& r = st.registry->get(hits[i]);
            if (std::strcmp(r.data_type, "Timeline") == 0) return lsd::editor::open_timeline_editor(st, r);
        }
        LSD_LOG_WARN("editor", "timeline.Open: no Timeline named like '%s'", args);
    }, &s, "Open a Timeline in the Timeline window");
    console::add_command("ui.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        lsd::usize hit = 0;
        if (st.registry->search(args, lsd::asset::AssetType::UiDocument, nullptr, &hit, 1) == 1 || st.registry->search(args, lsd::asset::AssetType::UiStyle, nullptr, &hit, 1) == 1) {
            lsd::editor::open_ui_editor(st, st.registry->get(hit));
        } else {
            LSD_LOG_WARN("editor", "ui.Open: no UI document or style named like '%s'", args);
        }
    }, &s, "Open a UI document (.rml) or style (.rcss) in the UI window");
    console::add_command("input.Hold", [](void*, const char* args) {  // tests / the MCP bridge drive Play
        // The key's name may have spaces ("Left Shift"); a number at the end is the seconds.
        char name[32] = "";
        float seconds = 1.0f;
        std::snprintf(name, sizeof name, "%s", args);
        if (char* last = std::strrchr(name, ' '); last && std::sscanf(last + 1, "%f", &seconds) == 1) *last = '\0';
        if (!name[0]) return;
        const char* const mouse[3] = {"Left Mouse Button", "Middle Mouse Button", "Right Mouse Button"};
        for (int b = 0; b < 3; ++b) {
            if (std::strcmp(name, mouse[b]) != 0) continue;
            g_mouse_held_until[b] = SDL_GetTicks() + static_cast<lsd::u64>(seconds * 1000.0f);
            return;
        }
        const SDL_Scancode k = SDL_GetScancodeFromName(name);
        if (k == SDL_SCANCODE_UNKNOWN) {
            LSD_LOG_WARN("input", "input.Hold: no key named %s (W, Space, E...)", name);
            return;
        }
        g_held_until[k] = SDL_GetTicks() + static_cast<lsd::u64>(seconds * 1000.0f);
    }, nullptr, "input.Hold <key> <seconds>: hold a key or mouse button for the game (tests), e.g. input.Hold W 2, input.Hold Right Mouse Button 1");
    console::add_command("ui.Keys", [](void*, const char* args) {  // tests: "down down enter", 0.8 s later
        static char keys[128];
        std::snprintf(keys, sizeof keys, "%s", args);
        SDL_AddTimer(800, [](void*, SDL_TimerID, Uint32) -> Uint32 {
            char copy[128];
            std::snprintf(copy, sizeof copy, "%s", keys);
            for (char* word = std::strtok(copy, " "); word; word = std::strtok(nullptr, " ")) {
                const SDL_Keycode k = std::strcmp(word, "down") == 0 ? SDLK_DOWN : std::strcmp(word, "up") == 0 ? SDLK_UP : std::strcmp(word, "enter") == 0 ? SDLK_RETURN
                                    : SDL_GetKeyFromName(word);  // any SDL key name: f5, delete, a...
                int window_count = 0;
                SDL_Window** windows = SDL_GetWindows(&window_count);
                const SDL_WindowID window = windows && window_count > 0 ? SDL_GetWindowID(windows[0]) : 0;  // ImGui ignores keys for no window
                SDL_free(windows);
                for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
                    SDL_Event e{};
                    e.type = type;
                    e.key.windowID = window;
                    e.key.key = k;
                    e.key.scancode = SDL_GetScancodeFromKey(k, nullptr);
                    e.key.down = type == SDL_EVENT_KEY_DOWN;
                    SDL_PushEvent(&e);
                }
            }
            return 0;
        }, nullptr);
    }, &s, "ui.Keys <down|up|enter|SDL key name ...>: keys for the UI, 0.8 s later (tests)");
    console::add_command("ui.Drag", [](void*, const char* args) {  // tests: a real LMB drag through SDL's queue
        struct Drag {
            float x0, y0, x1, y1;
            SDL_Keycode modifier;
        };
        auto* d = new Drag{};
        char mod[16] = "";
        if (std::sscanf(args, "%f %f %f %f %15s", &d->x0, &d->y0, &d->x1, &d->y1, mod) < 4) {
            delete d;
            return;
        }
        d->modifier = std::strcmp(mod, "shift") == 0 ? SDLK_LSHIFT : std::strcmp(mod, "alt") == 0 ? SDLK_LALT : std::strcmp(mod, "ctrl") == 0 ? SDLK_LCTRL : SDLK_UNKNOWN;
        SDL_AddTimer(800, [](void* user, SDL_TimerID, Uint32) -> Uint32 {
            const Drag d = *static_cast<const Drag*>(user);
            delete static_cast<const Drag*>(user);
            int window_count = 0;
            SDL_Window** windows = SDL_GetWindows(&window_count);
            const SDL_WindowID window = windows && window_count > 0 ? SDL_GetWindowID(windows[0]) : 0;
            SDL_free(windows);
            const SDL_Keymod held = d.modifier == SDLK_LSHIFT ? SDL_KMOD_LSHIFT : d.modifier == SDLK_LALT ? SDL_KMOD_LALT : d.modifier == SDLK_LCTRL ? SDL_KMOD_LCTRL : SDL_KMOD_NONE;
            auto key = [&](bool down) {
                if (d.modifier == SDLK_UNKNOWN) return;
                SDL_Event e{};
                e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.windowID = window;
                e.key.key = d.modifier;
                e.key.scancode = SDL_GetScancodeFromKey(d.modifier, nullptr);
                e.key.mod = down ? held : SDL_KMOD_NONE;
                e.key.down = down;
                SDL_PushEvent(&e);
            };
            auto move = [&](float x, float y) {
                SDL_Event e{};
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.windowID = window;
                e.motion.x = x;
                e.motion.y = y;
                SDL_PushEvent(&e);
            };
            auto button = [&](bool down, float x, float y) {  // where the mouse is, like real button events (the game UI reads it)
                SDL_Event e{};
                e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.windowID = window;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = down;
                e.button.x = x;
                e.button.y = y;
                SDL_PushEvent(&e);
            };
            move(d.x0, d.y0);
            SDL_Delay(150);
            key(true);
            SDL_Delay(50);
            button(true, d.x0, d.y0);
            for (int i = 1; i <= 20; ++i) {  // a steady hand: 20 steps over about a second
                SDL_Delay(50);
                move(d.x0 + (d.x1 - d.x0) * static_cast<float>(i) / 20.0f, d.y0 + (d.y1 - d.y0) * static_cast<float>(i) / 20.0f);
            }
            SDL_Delay(150);
            button(false, d.x1, d.y1);
            SDL_Delay(50);
            key(false);
            return 0;
        }, d);
    }, &s, "ui.Drag <x0> <y0> <x1> <y1> [shift|alt|ctrl]: an LMB drag, holding the key, 0.8 s later (tests)");
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
            SDL_Delay(100);  // like a hand: hover first, then press (tabs select on press, buttons on release)
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
    console::add_command("play", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        if (std::strcmp(args, "window") == 0) st.play_window_requested = true;  // Play in a new window
        else if (std::strcmp(args, "close") == 0) st.close_play_window = true;
        else st.play_requested = true;
    }, &s, "play: Play-in-Editor; play window: in a new window (Alt+N); play close: close that window");
    console::add_command("simulate", [](void* user, const char*) {
        auto& st = *static_cast<EditorState*>(user);
        st.play_requested = st.simulate_requested = true;
    }, &s, "Start Simulate: the level runs with the editor camera (Alt+S)");
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
    // Listings for tools (the Claude MCP plugin): answered to the remote client, else the Output Log.
    console::add_command("scene.List", [](void* user, const char*) {
        const auto& st = *static_cast<EditorState*>(user);
        lsd::u32 type_count = 0;
        const lsd::reflect::TypeInfo* const* types = lsd::world::component_types(type_count);
        std::string out;
        for (lsd::u32 slot = 0; slot < st.world->slot_count(); ++slot) {
            const lsd::world::Entity e = st.world->entity_at_slot(slot);
            if (!e.valid()) continue;
            const lsd::world::Entity parent = st.world->parent(e);
            const lsd::Mat4 m = st.world->world_matrix(e);
            char line[200];
            std::snprintf(line, sizeof line, "%s  parent=%s  at=(%.2f, %.2f, %.2f)  [", st.world->name(e), parent.valid() ? st.world->name(parent) : "-",
                          static_cast<double>(m.at(0, 3)), static_cast<double>(m.at(1, 3)), static_cast<double>(m.at(2, 3)));
            out += line;
            const char* separator = "";
            for (lsd::u32 i = 0; i < type_count; ++i) {
                if (!st.world->get(e, *types[i])) continue;
                out += separator;
                out += types[i]->name;
                separator = ", ";
            }
            out += "]\n";
        }
        lsd::editor::remote::print(out.empty() ? "(the level is empty)" : out.c_str());
    }, &s, "Every object in the level: name, parent, world position, components");
    console::add_command("asset.List", [](void* user, const char* args) {
        const auto& st = *static_cast<EditorState*>(user);
        std::string filter(args), out;
        for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (lsd::usize i = 0; i < st.registry->count(); ++i) {
            const lsd::asset::AssetRecord& r = st.registry->get(i);
            char line[420];
            std::snprintf(line, sizeof line, "%s  %s%s%s  Content/%s\n", r.name, lsd::asset::to_string(r.type), r.data_type[0] ? ":" : "", r.data_type, r.path);
            std::string lower(line);
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (filter.empty() || lower.find(filter) != std::string::npos) out += line;
        }
        lsd::editor::remote::print(out.empty() ? "(no assets match)" : out.c_str());
    }, &s, "asset.List [text]: every asset (name, type, path), or those whose line contains text");
    console::add_command("BuildReflections", [](void* user, const char*) { static_cast<EditorState*>(user)->capture_reflections = true; }, &s,
                         "BuildReflections: capture every Reflection Capture again (Build > Build Reflection Captures)");
    console::add_command("screenshot", [](void*, const char*) { g_screenshot = true; }, &s,
                         "screenshot: save the next frame to Saved/screenshot.bmp (OpenGL or Vulkan)");
    console::add_command("assistant.Ask", [](void* user, const char* args) {
        lsd::editor::assistant::ask(*static_cast<EditorState*>(user), args);
    }, &s, "assistant.Ask <message>: send a message to LSD Assistant, as if typed in its chat");
    console::add_command("assistant.Lesson", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        std::string report;
        lsd::editor::assistant::apply_lesson(st, static_cast<lsd::u32>(std::atoi(args)), report);
        lsd::editor::remote::print(report.c_str());
    }, &s, "assistant.Lesson <n>: apply LSD Assistant lesson n (0: gun on a hand socket) to the selected character");
    console::add_command("asset.Open", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        for (lsd::usize i = 0; i < st.registry->count(); ++i) {
            const lsd::asset::AssetRecord& r = st.registry->get(i);
            if (std::strcmp(r.name, args) != 0) continue;
            lsd::editor::remote::print(lsd::editor::open_asset(st, r) ? "opened" : "nothing opens this type");
            return;
        }
        lsd::editor::remote::print("no asset with that name (asset.List)");
    }, &s, "asset.Open <name>: open an asset in its editor, like a double-click in the Content Browser");
    console::add_command("asset.ImportMaterials", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        for (lsd::usize i = 0; i < st.registry->count(); ++i) {
            const lsd::asset::AssetRecord r = st.registry->get(i);
            if (r.type != lsd::asset::AssetType::Mesh || std::strcmp(r.name, args) != 0) continue;
            lsd::editor::MaterialImportReport report;
            lsd::editor::import_mesh_materials(*st.registry, r, "", false, report, st.status, sizeof st.status);
            lsd::editor::remote::print(st.status);
            return;
        }
        lsd::editor::remote::print("no mesh with that name (asset.List)");
    }, &s, "asset.ImportMaterials <mesh>: Import Materials and Textures for a mesh (as its Content Browser menu)");
    console::add_command("mode", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        auto same = [](const char* a, const char* b) {
            for (; *a && *b; ++a, ++b) {
                if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
            }
            return *a == *b;
        };
        for (int m = 0; m <= static_cast<int>(lsd::editor::EditorMode::Paper2D); ++m) {
            const auto mode = static_cast<lsd::editor::EditorMode>(m);
            if (same(lsd::editor::mode_name(mode), args) || (mode == lsd::editor::EditorMode::Modeling && same(args, "Modeling")) ||
                (mode == lsd::editor::EditorMode::Animation && same(args, "Animation"))) {
                st.mode = mode;
                return;
            }
        }
        lsd::editor::remote::print("Modes: Select, Modeling, Foliage, Landscape, Animation, PCG, VFX, 2D");
    }, &s, "mode <name>: the Level tab's mode (Select, Modeling, Foliage, Landscape, Animation, PCG, VFX)");
    console::add_command("loc.Gather", [](void* user, const char*) {
        auto& st = *static_cast<EditorState*>(user);
        bool changed = false;
        lsd::editor::gather_project_text(st.project, *st.registry, st.status, sizeof st.status, changed);
        if (changed) lsd::reflect::save_file(st.project, "Config/project.json");  // the new string table
        lsd::editor::remote::print(st.status);
    }, &s, "Gather Text: adds the keys the project shows to its string table");
    console::add_command("template.New", [](void* user, const char* args) {
        auto& st = *static_cast<EditorState*>(user);
        for (int t = 0; t < static_cast<int>(lsd::editor::Template::Count); ++t) {
            const auto kind = static_cast<lsd::editor::Template>(t);
            const char* name = lsd::editor::template_name(kind);
            bool same = std::strlen(name) == std::strlen(args);
            for (lsd::usize i = 0; same && name[i]; ++i) same = std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(args[i]));
            if (same) {
                st.show_template_picker = false;
                return lsd::editor::build_template(st, kind, nullptr);
            }
        }
        std::string names = "Templates:";
        for (int t = 0; t < static_cast<int>(lsd::editor::Template::Count); ++t) names += std::string(t ? ", " : " ") + lsd::editor::template_name(static_cast<lsd::editor::Template>(t));
        lsd::editor::remote::print(names.c_str());
    }, &s, "template.New <name>: a new level from a template (Empty, First Person, Third Person, Platformer, Side Scrolling, 2D, 2.5D)");
    console::add_command("workspace", [](void* user, const char* args) {
        std::snprintf(static_cast<EditorState*>(user)->workspace_request, sizeof EditorState::workspace_request, "%s", args);
    }, &s, "workspace <name>: switch workspace tab (Layout, Modeling, Shading, Animation, Scripting, UI, Effects...)");
    console::add_command("game.Compile",[](void*, const char*) { lsd::editor::game_module::compile(); }, &s,
                         "Tools > Compile Game Code: build Source/ into Binaries/Game.dll (it reloads by itself)");
    console::add_command("game.NewScript", [](void*, const char* args) {  // "Door", "Door c", "Health component", "Health c component"
        char name[64] = "", word1[16] = "", word2[16] = "";
        std::sscanf(args, "%63s %15s %15s", name, word1, word2);
        auto said = [&](const char* w) { return std::strcmp(word1, w) == 0 || std::strcmp(word2, w) == 0; };
        const bool c = said("c") || said("C");
        char message[300];
        if (lsd::editor::game_module::new_script(name, c, message, sizeof message, said("component"))) {
            LSD_LOG_INFO("game", "%s", message);
            lsd::editor::game_module::compile();
        } else {
            LSD_LOG_WARN("game", "%s", message);
        }
    }, &s, "game.NewScript <Name> [c] [component]: Tools > New C / C++ Script (Source/<Name>.cpp, .c with c; a component of your own with component), then compile");
    console::add_command("game.Scripts", [](void*, const char*) {
        const lsd::script::NativeModule& m = lsd::editor::game_module::module();
        std::string out = lsd::editor::game_module::loaded() ? "" : "(no C / C++ scripts loaded: compile Source/ first)\n";
        for (lsd::u32 i = 0; i < m.count; ++i) out += std::string(m.names[i]) + "\n";
        lsd::editor::remote::print(out.empty() ? "(the game code has no scripts)" : out.c_str());
    }, &s, "The C / C++ scripts the compiled game code has");
    console::add_command("log.Tail", [](void*, const char* args) {
        const int n = args[0] ? std::atoi(args) : 50;
        lsd::editor::remote::print(lsd::editor::recent_log(n > 0 ? static_cast<lsd::u32>(n) : 50u).c_str());
    }, &s, "log.Tail [n]: the last n Output Log lines (default 50)");
}

// Meshes imported since the last scan: their Content path and the folder they came from (where
// their textures are), for Import Materials and Textures once the registry knows them.
std::vector<std::pair<std::string, std::string>> g_pending_material_imports;

// After the registry scan that follows an import: each new mesh's materials and textures (only when
// its textures are found, so animation-only files bring nothing).
void import_pending_materials(lsd::asset::AssetRegistry& registry, char* status, lsd::usize capacity) {
    for (const auto& [path, from] : g_pending_material_imports) {
        const std::string relative = fs::relative(path, kContentDir).generic_string();
        for (lsd::usize i = 0; i < registry.count(); ++i) {
            if (relative != registry.get(i).path) continue;
            const lsd::asset::AssetRecord mesh = registry.get(i);
            lsd::editor::MaterialImportReport report;
            char message[256];
            if (lsd::editor::import_mesh_materials(registry, mesh, from.c_str(), true, report, message, sizeof message)) std::snprintf(status, capacity, "%s", message);
            break;
        }
    }
    g_pending_material_imports.clear();
}

// Copies a file dropped from Explorer into Content/<folder>/, renaming on clash ("Chair_1.fbx").
// The registry picks it up on the next scan and writes its .meta.
bool import_file(const char* source, const char* content_folder, char* status, lsd::usize capacity) {
    const fs::path src(source);
    std::error_code dir_error;
    if (fs::is_directory(src, dir_error)) {  // a folder: everything importable in it, keeping its sub-folders
        lsd::u32 imported = 0, skipped = 0;
        for (const auto& entry : fs::recursive_directory_iterator(src, dir_error)) {
            if (!entry.is_regular_file(dir_error)) continue;
            const std::string ext = entry.path().extension().string();
            if (lsd::asset::type_from_extension(ext.c_str()) == lsd::asset::AssetType::Unknown) {
                if (ext != ".bin") ++skipped;  // a .gltf's buffer goes with it
                continue;
            }
            const fs::path relative = fs::relative(entry.path().parent_path(), src.parent_path(), dir_error);
            const std::string target = (fs::path(content_folder) / relative).generic_string();
            char ignored[300];
            imported += import_file(entry.path().string().c_str(), target.c_str(), ignored, sizeof ignored) ? 1u : 0u;
        }
        std::snprintf(status, capacity, "Imported %u file%s from %s%s", imported, imported == 1 ? "" : "s", src.filename().string().c_str(), skipped ? " (other files skipped)" : "");
        LSD_LOG_INFO("editor", "%s", status);
        return imported > 0;
    }
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
    if (lsd::asset::type_from_extension(src.extension().string().c_str()) == lsd::asset::AssetType::Mesh) {
        g_pending_material_imports.emplace_back(dest.generic_string(), fs::absolute(src, error).parent_path().string());
    }
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


// ScriptNode data assets into the visual-script node library (after every scan that changed something).
void register_script_nodes(const lsd::asset::AssetRegistry& registry) {
    lsd::script::clear_custom_nodes();
    for (lsd::usize i = 0; i < registry.count(); ++i) {
        const lsd::asset::AssetRecord& r = registry.get(i);
        char path[300];
        registry.source_path(r, path, sizeof path);
        if (r.type == lsd::asset::AssetType::Script && std::filesystem::path(path).extension() == ".js") {  // a .js file's `nodes`
            lsd::script::register_js_nodes(path, r.name);
            continue;
        }
        if (r.type != lsd::asset::AssetType::Data || std::strcmp(r.data_type, "ScriptNode") != 0) continue;
        lsd::world::ScriptNode node;
        if (!lsd::asset::load_data_asset(path, node)) continue;
        lsd::script::NodeDef def{};
        char id[128];
        std::snprintf(id, sizeof id, "Custom_%s", r.name);  // saved in graphs: renaming the asset breaks them, like a C++ rename
        def.id = id;
        def.title = r.name;
        def.category = node.category[0] ? node.category : "Custom";
        def.tooltip = node.tooltip;
        for (lsd::u32 k = 0; k < node.inputs.count && k < 6; ++k) def.inputs[k] = {node.inputs[k].name, static_cast<lsd::script::PinType>(node.inputs[k].type), node.inputs[k].default_value};
        for (lsd::u32 k = 0; k < node.outputs.count && k < 4; ++k) def.outputs[k] = {node.outputs[k].name, static_cast<lsd::script::PinType>(node.outputs[k].type), ""};
        def.code = node.code[0] ? node.code : "";
        def.output_code[0] = node.expression[0] ? node.expression : nullptr;
        lsd::script::register_node(def);
    }
    if (lsd::editor::game_module::loaded()) lsd::script::register_native_nodes(lsd::editor::game_module::module());  // C / C++ nodes
}

// Multiplayer Play in the editor: the other players' game windows (closed on Stop).
std::vector<SDL_Process*> g_play_clients;

// Play in a new window: the game as its own process on a saved copy of the level (closed with the editor).
SDL_Process* g_play_window = nullptr;
lsd::u64 g_play_window_deadline = 0;  // asked to close: ended for good if still running by then (ms)

// Asks the window to close (the game quits as on its own close button); `now`: end it at once (editor exit).
void close_play_window(bool now = false) {
    if (!g_play_window) return;
    SDL_KillProcess(g_play_window, now);
    if (now) {
        SDL_DestroyProcess(g_play_window);
        g_play_window = nullptr;
    } else if (!g_play_window_deadline) {
        g_play_window_deadline = SDL_GetTicks() + 2000;
    }
}

void stop_play_clients() {
    for (SDL_Process* p : g_play_clients) {
        SDL_KillProcess(p, false);
        SDL_DestroyProcess(p);
    }
    g_play_clients.clear();
}

// The built-in chat box (Project Settings > Chat > Chat Box): the last lines, Enter to type, Tab
// picks the channel among those the project turns on, Esc cancels.
void draw_chat_box(lsd::editor::PlaySession& play, const lsd::editor::ProjectSettings& project) {
    static bool typing = false;
    static int channel = 0;
    static char line[256] = "";
    const char* channels[4];
    int count = 0;
    if (project.chat_all) channels[count++] = "all";
    if (project.chat_team) channels[count++] = "team";
    if (project.chat_squad) channels[count++] = "squad";
    if (project.chat_proximity) channels[count++] = "proximity";
    if (!count) return;
    channel %= count;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(16.0f, io.DisplaySize.y - 250.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(440.0f, 220.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(typing ? 0.65f : 0.25f);
    ImGui::Begin("##chat", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const lsd::net::NetSession& net = play.network();
    const lsd::net::NetStats stats = net.stats();
    ImGui::TextDisabled("%s, player %u%s  %.0f ms", net.mode() == lsd::net::Mode::Server ? "Server" : "Client", net.player_id(), net.connected() ? "" : " (connecting)", stats.ping_ms);
    if (play.talking()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  Talking");
    }
    ImGui::BeginChild("##lines", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
    for (const auto& l : play.chat_log()) {
        const ImVec4 color = std::strcmp(l.channel, "team") == 0 ? ImVec4(0.45f, 0.8f, 1.0f, 1.0f)
                           : std::strcmp(l.channel, "squad") == 0 ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f)
                           : std::strcmp(l.channel, "proximity") == 0 ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f) : ImVec4(1, 1, 1, 1);
        ImGui::TextColored(color, "[%s] Player %u: %s", l.channel, l.from, l.text);
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (!typing && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !io.WantTextInput) typing = true;
    if (typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) channel = (channel + 1) % count;
        ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "[%s]", channels[channel]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##say", line, sizeof line, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (line[0]) play.send_chat(channels[channel], line);
            line[0] = '\0';
            typing = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) typing = false;
    } else {
        ImGui::TextDisabled("Enter: chat");
    }
    ImGui::End();
}

// File > Import...: SDL's file dialog answers on any thread; the main loop imports what it chose.
std::mutex g_import_mutex;
std::vector<std::string> g_import_queue;

void SDLCALL on_import_chosen(void*, const char* const* files, int) {
    if (!files) return;  // cancelled or failed
    std::lock_guard<std::mutex> lock(g_import_mutex);
    for (const char* const* f = files; *f; ++f) g_import_queue.emplace_back(*f);
}


// While a script waits at a breakpoint: keep the window alive and draw the debugger (and the graph
// editor, to show the stopped node) until it resumes. The game is frozen meanwhile.
struct DebugFrameContext {
    lsd::rhi::Device* device;
    SDL_Window* window;
    EditorState* state;
};
void debug_frame(void* user) {
    auto& c = *static_cast<DebugFrameContext*>(user);
    SDL_SetWindowRelativeMouseMode(c.window, false);  // a captured mouse could not reach the buttons
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            c.state->stop_requested = true;
            lsd::editor::debugger::resume(lsd::script::DebugAction::Continue);
        }
    }
    c.device->debug_ui_new_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    lsd::editor::debugger::draw_window(*c.state, &c.state->show_debugger);
    lsd::editor::draw_graph_editor(*c.state);
    ImGui::Render();
    const lsd::f32 clear[4] = {0.05f, 0.02f, 0.02f, 1.0f};
    c.device->begin_frame(clear);
    c.device->render_debug_ui();
    c.device->end_frame();
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
    s.capture_reflections = true;  // the new level's captures
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
    const char* connect_arg = nullptr; // -connect=<host[:port]>: multiplayer, join that server
    const char* net_level_arg = nullptr; // -netlevel=<name>: the level's name for multiplayer (Play's extra windows play a copy)
    bool dedicated_arg = false;        // -dedicated: host without a player of its own
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
        else if (std::strncmp(argv[i], "-connect=", 9) == 0) connect_arg = argv[i] + 9;
        else if (std::strncmp(argv[i], "-netlevel=", 10) == 0) net_level_arg = argv[i] + 10;
        else if (std::strcmp(argv[i], "-server") == 0) {}  // multiplayer host (a listen server): already the default without -connect
        else if (std::strcmp(argv[i], "-dedicated") == 0) dedicated_arg = true;
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
        // Shipping (Project Settings > Packaging): what players get takes no remote console, no -exec
        // lines and no smoke tests, so nobody drives the game from outside.
        std::ifstream marker(lsd::editor::kGameMarker);
        const std::string text((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        if (text.find("\"shipping\": true") != std::string::npos) {
            remote_arg = 0;
            exec_count = 0;
            smoke_arg = 0.0f;
        }
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

    SDL_Window* splash = game_mode ? lsd::editor::open_splash() : nullptr;  // the packaged game's splash, up while it loads
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
    lsd::editor::load_keymap(state.settings);
    lsd::reflect::load_file(state.project, "Config/project.json");
    state.game_mode = game_mode;
    if (game_mode) SDL_SetWindowTitle(window, state.project.project_name);

    // Types that can be created as data assets (Content Browser > New Data Asset).
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::input::InputAction>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::input::InputMappingContext>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::GameMode>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::PhysicalMaterial>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::Curve>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::CurveVector>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::CurveColor>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::UserEnum>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::UserStruct>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::ScriptNode>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::Timeline>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::world::Flipbook>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::render::Material>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::render::MaterialInstance>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundClass>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundAttenuation>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundConcurrency>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::SoundMix>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::audio::Submix>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::anim::AnimGraph>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::anim::MotionDatabase>());
    lsd::asset::register_data_type(lsd::reflect::type_of<lsd::particles::ParticleSystem>());

    lsd::asset::AssetRegistry registry(allocator, kContentDir);
    lsd::world::set_prefab_resolver([](void* user, const char* guid_text, char* out, lsd::usize capacity) {  // nested prefabs refresh from their files
        const auto& reg = *static_cast<const lsd::asset::AssetRegistry*>(user);
        lsd::asset::Guid guid;
        const lsd::asset::AssetRecord* r = lsd::asset::parse_guid(guid_text, guid) ? reg.find(guid) : nullptr;
        if (!r || r->type != lsd::asset::AssetType::Prefab) return false;
        reg.source_path(*r, out, capacity);
        return true;
    }, &registry);
    if (import_arg) import_file(import_arg, "", state.status, sizeof state.status);
    registry.scan();
    import_pending_materials(registry, state.status, sizeof state.status);
    register_script_nodes(registry);
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
    auto* animations = new lsd::editor::AnimationPlayer(*device);  // deleted before the device (morphed vertex buffers)
    auto* effects = new lsd::editor::ParticlePlayer(*device, allocator, registry, *texture_cache);  // deleted before the textures
    g_skinned = skinned_cache;
    g_materials = &materials;
    auto* decal_atlas = new lsd::editor::DecalAtlas(*device, allocator, registry);  // deleted before the device
    g_decals = decal_atlas;
    g_animations = animations;
    auto* thumbnail_memory = allocator.allocate(sizeof(lsd::editor::ThumbnailCache), alignof(lsd::editor::ThumbnailCache));
    lsd::editor::ThumbnailCache& thumbnails = *new (thumbnail_memory) lsd::editor::ThumbnailCache(*device, allocator, registry, kDataDir);
    state.world = &world;
    state.history = &history;
    state.meshes = &meshes;
    state.thumbnails = &thumbnails;
    thumbnails.set_materials(&materials);
    if (!game_mode) lsd::editor::source_control::start();  // Git badges in the Content Browser
    state.textures = texture_cache;
    state.skinned = skinned_cache;
    state.materials = &materials;
    state.animations = animations;
    state.device = device;

    const char* startup_level = level_arg              ? level_arg
                                : game_mode              ? state.project.game_default_level
                                : state.settings.last_level[0] ? state.settings.last_level
                                                         : state.project.editor_startup_level;
    // Game code first: its own components must be known before a level that uses them is read.
    lsd::editor::game_module::load();  // the project's C / C++ scripts and components (Binaries/Game.dll), if any
    if (!startup_level[0] || !fs::exists(startup_level) || !open_level(state, startup_level)) {
        new_level(state);
        // Nothing saved to come back to (first launch): pick a template. A saved project opens as it was.
        state.show_template_picker = !game_mode && !level_arg;
    }
    if (state.settings.viewport_kinds) lsd::editor::set_viewport_kinds(state, state.settings.viewport_kinds);  // the views the project was left with

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
    effects->set_meshes(&meshes);  // the Mesh renderer
    auto* particle_editor = new lsd::editor::ParticleEditor(*device, allocator, *effects, grid);  // previews with the level's grid
    state.particle_editor = particle_editor;
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
        const lsd::editor::Scalability scalability = lsd::editor::resolve_scalability(user_settings);
        lsd::editor::set_shadow_quality(scalability.shadows);
        lsd::editor::set_post_quality(scalability.post);
        g_view_distance_scale = lsd::editor::view_distance_scale(scalability.view_distance);
        lsd::particles::set_spawn_scale(lsd::editor::effects_scale(scalability.effects));
        sounds.set_class_volume("Master", user_settings.master_volume);
        sounds.set_class_volume("Music", user_settings.music_volume, false);  // projects without these classes: nothing to set
        sounds.set_class_volume("SFX", user_settings.effects_volume, false);
        if (game_mode) SDL_SetWindowFullscreen(window, user_settings.fullscreen);
    };
    apply_user_settings();
    play.set_user_settings(&user_settings);
    state.user_settings = &user_settings;
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
    play.set_base_mix(state.project.base_sound_mix);
    lsd::ui::set_translator([](void* user, const char* text) { return static_cast<const lsd::asset::Localization*>(user)->find(text); }, &localization);
    play.set_animations(animations);
    play.set_skinned(skinned_cache);
    static DebugFrameContext debug_context;
    debug_context = {device, window, &state};
    register_script_nodes(registry);  // with the game code's nodes (loaded before the startup level)
    play.scripts().set_native_module(&lsd::editor::game_module::module());
    if (!game_mode) lsd::editor::debugger::attach(play.scripts());  // a packaged game has no debugger (and runs scripts uninstrumented)
    lsd::editor::debugger::set_frame_callback(debug_frame, &debug_context);
    lsd::console::add_command("lua", [](void* user, const char* args) {
        std::string code(args);
        for (char& c : code) {
            if (c == '\x1f') c = '\n';  // line breaks of multi-line code sent on one console line (the MCP bridge)
        }
        if (!static_cast<lsd::editor::PlaySession*>(user)->run_lua(code.c_str())) LSD_LOG_WARN("script", "lua: runs while playing (see the Output Log for errors)");
    }, &play, "Run Lua while playing, e.g. lua lsd.log(lsd.time())");
    lsd::Array<char> play_snapshot(allocator);  // the level before Play, restored on Stop
    lsd::f32 mouse_dx = 0.0f, mouse_dy = 0.0f;
    SDL_Gamepad* gamepad = nullptr;  // the first one plugged in drives the player
    char title[400] = "";

    bool running = true;
    bool minimized = false;
    lsd::rhi::TextureHandle shot_target, shot_scene;  // HighResShot, read back after the frame
    lsd::rhi::TextureHandle reflection_faces[lsd::render::kMaxReflections * 6], reflection_atlas;  // reflection captures
    lsd::f32 reflection_probes[lsd::render::kMaxReflections][4] = {};
    lsd::u32 reflection_capture_count = 0;  // captures rendered this frame, read back after it
    lsd::f32 shot_size[2] = {};
    lsd::u64 last_scan = SDL_GetTicks();
    lsd::u64 last_tick = SDL_GetTicksNS();
    lsd::u32 mcp_tried_port = 0;  // Claude MCP plugin: the port last tried, so a busy one is not retried every frame
    for (long frame = 0; running && frame != max_frames;) {
        SDL_Event event;
        if (!game_mode && remote_arg == 0) {  // -remote keeps its own listener
            const lsd::u32 port = state.settings.mcp_port;
            if (!state.settings.mcp_enabled) {
                if (lsd::editor::remote::listening()) lsd::editor::remote::stop_listening();
                mcp_tried_port = 0;
            } else if (mcp_tried_port != port && port > 0 && port < 65536) {
                if (lsd::editor::remote::listening()) lsd::editor::remote::stop_listening();
                mcp_tried_port = port;
                lsd::editor::remote::listen(static_cast<lsd::u16>(port));
            }
        }
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
                    if (import_file(event.drop.data, state.content_folder, state.status, sizeof state.status)) {
                        registry.scan();
                        import_pending_materials(registry, state.status, sizeof state.status);
                    }
                    break;
                default: break;
            }
        }
        if (minimized) { SDL_Delay(16); continue; }
        const lsd::u64 now = SDL_GetTicksNS();
        const lsd::f32 dt = static_cast<lsd::f32>(static_cast<double>(now - last_tick) * 1e-9);
        last_tick = now;

        // Pick up new, changed and deleted files once a second; recompile what changed.
        if (SDL_GetTicks() - last_scan > 1000 || state.reload_assets_now) {
            state.reload_assets_now = false;
            if (!game_mode) lsd::editor::live_link_sync(state.settings.live_link_folder, kContentDir, state.status, sizeof state.status);  // copied files show up in this scan
            if (registry.scan_async()) register_script_nodes(registry);
            meshes.hot_reload();
            texture_cache->check_files();
            skinned_cache->check_files();
            effects->check_files();
            materials.check_files();
            if (!game_mode && lsd::editor::game_module::changed() && !lsd::editor::game_module::compiling()) {  // a rebuilt Game.dll
                play.scripts().stop_natives();  // their code is in the old DLL
                lsd::editor::game_module::load();
                register_script_nodes(registry);  // its nodes may have changed
                play.scripts().start_natives();  // during Play: the behaviours start over with the new code
            }
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

        lsd::world::update_spline_meshes(world);  // Spline Meshes follow their curves (only rebuilt on change)
        lsd::editor::update_landscapes(world, meshes);  // landscape meshes follow their heights (rebuilt on change)
        if (state.world != &world) lsd::editor::update_landscapes(*state.world, meshes);  // Play's copy
        FrameTimer timer;
        if (state.load_layout[0]) lsd::editor::apply_layout_file(state.load_layout, state.status, sizeof state.status);
        state.load_layout[0] = '\0';
        device->debug_ui_new_frame();
        {  // While the game has the mouse the editor ignores it: no cursor (the SDL3 backend would show it again every frame), no hovering panels.
            ImGuiIO& gio = ImGui::GetIO();
            constexpr ImGuiConfigFlags kGameMouse = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
            if (state.playing && state.mouse_captured && !state.ejected) {
                gio.ConfigFlags |= kGameMouse;
                SDL_HideCursor();
            } else if (gio.ConfigFlags & kGameMouse) {
                gio.ConfigFlags &= ~kGameMouse;
                SDL_ShowCursor();
            }
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        {
            LSD_PROFILE_SCOPE("editor ui");
            if (game_mode) lsd::editor::draw_game(state);
            else lsd::editor::draw_editor(state);
            if (state.playing && play.network().active() && state.project.chat_box) draw_chat_box(play, state.project);
            ImGui::Render();
        }
        state.stat_ui_ms = timer.lap();
        if (state.import_requested) {
            state.import_requested = false;
            static const SDL_DialogFileFilter filters[] = {
                {"Meshes, textures, sounds, tables", "fbx;gltf;glb;png;jpg;jpeg;tga;bmp;psd;wav;ogg;flac;mp3;csv"},
                {"All files", "*"},
            };
            SDL_ShowOpenFileDialog(on_import_chosen, nullptr, window, filters, 2, nullptr, true);
        }
        {
            std::vector<std::string> chosen;
            {
                std::lock_guard<std::mutex> lock(g_import_mutex);
                chosen.swap(g_import_queue);
            }
            lsd::u32 imported = 0;
            for (const std::string& file : chosen) imported += import_file(file.c_str(), state.content_folder, state.status, sizeof state.status) ? 1u : 0u;
            if (chosen.size() > 1) std::snprintf(state.status, sizeof state.status, "Imported %u of %zu files", imported, chosen.size());
            if (imported) {
                registry.scan();
                import_pending_materials(registry, state.status, sizeof state.status);
            }
        }
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
        if (g_lightmaps_changed) upload_lightmaps(*device, meshes);
        play.set_workspace(game_mode ? "" : state.settings.workspace);  // lsd.editor_workspace
        if (g_lighting_build) {
            LightingBuild& build = *g_lighting_build;
            const lsd::u32 total = build.total.load() ? build.total.load() : 1u;
            state.build_lighting_progress = static_cast<float>(build.done.load()) / static_cast<float>(total);
            std::snprintf(state.status, sizeof state.status, "Building lighting... %d%%", static_cast<int>(state.build_lighting_progress * 100.0f));
            if (build.finished) {
                build.thread.join();
                g_level_lighting = std::move(build.result);
                lsd::editor::set_baked_lighting(&g_level_lighting);
                g_lightmaps_changed = true;
                const bool saved = lsd::editor::save_baked_lighting(build.path, g_level_lighting);
                std::snprintf(state.status, sizeof state.status, saved ? "Lighting built: %u probes, %u lightmapped objects in %.1f s (%s)" : "Lighting built (%u probes, %u lightmapped objects, %.1f s) but %s could not be written",
                              g_level_lighting.probe_count(), static_cast<unsigned>(g_level_lighting.objects.size()), static_cast<double>(SDL_GetTicks() - build.started) / 1000.0, build.path);
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

        // lsd.open_level: another level inside the same Play (Unreal's Open Level). Stop still reverts to the
        // level being edited (play_snapshot); game_instance / player_state travel with the scripts.
        // shortcut: the new level's baked lighting (BuiltData) is not loaded; load it here when travel matters for looks.
        if (char travel_to[300]; state.playing && play.take_level_request(travel_to, sizeof travel_to)) {
            play.stop();
            state.kept_count = 0;  // Keep Simulation Changes names objects of the old level
            // "*": the level Play started on again (lsd.net_host / net_join / net_leave restart in their mode)
            const bool again = std::strcmp(travel_to, "*") == 0;
            if (again ? lsd::world::load_level(world, play_snapshot.data(), play_snapshot.size()) : lsd::world::load_level_file(world, travel_to)) {
                play.start(world, meshes, registry, state.camera.position, state.camera.yaw, state.camera.pitch, true);
                animations->reset();
                effects->reset();
                if (again) LSD_LOG_INFO("editor", "Restarted the level (multiplayer host / join / leave)");
                else LSD_LOG_INFO("editor", "Opened %s (open_level)", travel_to);
            } else {
                LSD_LOG_ERROR("editor", "open_level: could not read %s", travel_to);
                state.stop_requested = true;
            }
        }
        // Play in a new window (Alt+N): the level as it is now, unsaved edits too, played by a game process.
        if (std::exchange(state.play_window_requested, false) && !g_play_window && !state.playing) {
            lsd::Array<char> text(allocator);
            lsd::world::save_level(world, text);
            const std::string level = "Saved/PlayInWindow" + std::string(lsd::world::kLevelExtension);
            if (FILE* f = std::fopen(level.c_str(), "wb")) {
                std::fwrite(text.data(), 1, text.size(), f);
                std::fclose(f);
            }
            const std::string level_flag = "-level=" + level;
            const char* args[] = {argv[0], "-game", level_flag.c_str(), no_sound ? "-nosound" : nullptr, nullptr};
            g_play_window = SDL_CreateProcess(args, false);
            if (g_play_window) LSD_LOG_INFO("editor", "Playing in a new window");
            else LSD_LOG_ERROR("editor", "Could not open the play window: %s", SDL_GetError());
        }
        if (std::exchange(state.close_play_window, false)) close_play_window();
        if (state.show_multiplayer) {  // the Multiplayer panel's view of the session (about 4 times a second)
            static lsd::u64 next_refresh = 0;
            if (SDL_GetTicks() >= next_refresh || state.net_codes_wanted) {
                next_refresh = SDL_GetTicks() + 250;
                auto& v = state.net;
                const lsd::net::NetSession& net = play.network();
                const lsd::net::NetStats st = net.stats();
                v.mode = !state.playing ? 0 : net.mode() == lsd::net::Mode::Server ? 1 : net.mode() == lsd::net::Mode::Client ? 2 : 0;
                v.player = net.active() ? net.player_id() : 0;
                v.connected = net.active() && net.connected();
                v.players = st.players;
                v.replicated = st.replicated;
                v.ping_ms = st.ping_ms;
                v.in_kbs = st.bytes_in_per_second / 1024.0f;
                v.out_kbs = st.bytes_out_per_second / 1024.0f;
                if (v.mode == 1 && state.net_codes_wanted) play.join_codes(v.codes, sizeof v.codes, true);
                else if (v.mode == 1 && v.codes[0]) play.join_codes(v.codes, sizeof v.codes, false);  // UPnP finishing
                else if (v.mode != 1) v.codes[0] = '\0';
                state.net_codes_wanted = false;
            }
        }
        if (g_play_window && g_play_window_deadline && SDL_GetTicks() > g_play_window_deadline) close_play_window(true);  // did not close when asked
        if (g_play_window && SDL_WaitProcess(g_play_window, false, nullptr)) {  // its window was closed (Esc in the game)
            SDL_DestroyProcess(g_play_window);
            g_play_window = nullptr;
        }
        if (!g_play_window) g_play_window_deadline = 0;
        state.play_window_open = g_play_window != nullptr;
        if (state.play_requested && !state.playing) {
            const lsd::u64 snapshot_start = SDL_GetTicksNS();
            lsd::world::save_level(world, play_snapshot);
            const double snapshot_ms = static_cast<double>(SDL_GetTicksNS() - snapshot_start) * 1e-6;
            if (snapshot_ms > 100.0) LSD_LOG_WARN("editor", "Saving the level for Play took %.0f ms", snapshot_ms);  // only worth a line when slow
            // Multiplayer (Project Settings): join with -connect, else host; the editor's Play also
            // opens a game window per extra player, joining it, on the level as it is now.
            lsd::editor::NetPlay net_play;
            const lsd::editor::ProjectSettings& pr = state.project;
            net_play.enabled = pr.multiplayer;
            net_play.allowed = pr.multiplayer;
            net_play.host = connect_arg == nullptr && !state.net_join[0];
            net_play.dedicated = dedicated_arg;
            if (connect_arg) std::snprintf(net_play.address, sizeof net_play.address, "%s", connect_arg);
            if (state.net_join[0]) {  // the Multiplayer panel's Join: this Play is a client
                std::snprintf(net_play.address, sizeof net_play.address, "%s", state.net_join);
                state.net_join[0] = '\0';
            }
            net_play.settings.port = static_cast<lsd::u16>(pr.port);
            net_play.settings.max_players = pr.max_players;
            net_play.settings.tick_rate = pr.tick_rate;
            net_play.settings.interpolation_delay = pr.interpolation_ms * 0.001f;
            net_play.settings.chat_all = pr.chat_all;
            net_play.settings.chat_team = pr.chat_team;
            net_play.settings.chat_squad = pr.chat_squad;
            net_play.settings.chat_proximity = pr.chat_proximity;
            net_play.settings.proximity_range = pr.proximity_range;
            std::snprintf(net_play.avatar, sizeof net_play.avatar, "%s", pr.player_avatar);
            net_play.voice = pr.voice_chat && !no_sound;  // silent runs never open the microphone
            net_play.open_mic = pr.open_mic;
            net_play.settings.voice_team = pr.voice_team;
            net_play.settings.voice_all = pr.voice_all;
            net_play.settings.voice_squad = pr.voice_squad;
            net_play.settings.voice_proximity = pr.voice_proximity;
            std::snprintf(net_play.push_to_talk_key, sizeof net_play.push_to_talk_key, "%s", pr.push_to_talk_key);
            std::snprintf(net_play.talk_to_all_key, sizeof net_play.talk_to_all_key, "%s", pr.talk_to_all_key);
            play.set_network(net_play);
            {  // the level's asset name: players joining later follow the server to it
                std::string name = net_level_arg ? net_level_arg : state.level_path;
                name = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
                if (!net_level_arg && name.find('.') != std::string::npos) name.resize(name.rfind('.'));
                play.set_level_name(name.c_str());
            }
            if (pr.multiplayer && !game_mode && pr.play_players > 1 && net_play.host) {  // a joining Play opens no windows of its own
                const std::string level = "Saved/MultiplayerPlay" + std::string(lsd::world::kLevelExtension);
                if (FILE* f = std::fopen(level.c_str(), "wb")) {
                    std::fwrite(play_snapshot.data(), 1, play_snapshot.size(), f);
                    std::fclose(f);
                }
                char connect[96], level_flag[300], level_name_flag[96];
                std::string level_name = state.level_path;  // the windows play a copy of this level: the same one by name
                level_name = level_name.substr(level_name.find_last_of("/\\") == std::string::npos ? 0 : level_name.find_last_of("/\\") + 1);
                if (level_name.find('.') != std::string::npos) level_name.resize(level_name.rfind('.'));
                std::snprintf(level_name_flag, sizeof level_name_flag, "-netlevel=%s", level_name.c_str());
                std::snprintf(connect, sizeof connect, "-connect=127.0.0.1:%u", pr.port);
                std::snprintf(level_flag, sizeof level_flag, "-level=%s", level.c_str());
                for (lsd::u32 k = 1; k < pr.play_players; ++k) {
                    const char* args[] = {argv[0], "-game", level_flag, connect, level_name_flag, no_sound ? "-nosound" : nullptr, nullptr};
                    if (SDL_Process* p = SDL_CreateProcess(args, false)) g_play_clients.push_back(p);
                    else LSD_LOG_ERROR("net", "Could not open player %u's window: %s", k + 1, SDL_GetError());
                }
            }
            play.start(world, meshes, registry, state.camera.position, state.camera.yaw, state.camera.pitch);
            animations->reset();  // Play starts every animation from its beginning
            effects->reset();
            state.playing = true;
            state.paused = false;
            // Simulate (Unreal's Alt+S): the level runs while you keep the editor camera and selection.
            // shortcut: the player is still spawned (just not possessed); skip spawning it if that gets in the way.
            state.ejected = state.simulate_requested;
            state.mouse_captured = !state.ejected;
            LSD_LOG_INFO("editor", state.simulate_requested ? "Simulate" : "Play");
        }
        state.simulate_requested = false;
        state.play_requested = false;
        for (int i = 0; i < exec_count; ++i) lsd::console::execute(exec_args[i]);  // first frame: every command registered, -play started
        exec_count = 0;
        if (state.stop_requested && state.playing) {
            stop_play_clients();  // the other players' windows close with the session
            // Keep Simulation Changes: copy the kept objects' components before the level reverts.
            struct Kept {
                lsd::world::Entity entity;
                std::string name;
                std::vector<std::pair<const lsd::reflect::TypeInfo*, std::vector<lsd::u8>>> components;
            };
            std::vector<Kept> kept;
            for (lsd::u32 k = 0; k < state.kept_count; ++k) {
                const lsd::world::Entity e = state.kept[k];
                if (!world.alive(e)) continue;  // destroyed in Play: nothing to keep
                Kept item{e, world.name(e), {}};
                lsd::u32 type_count = 0;
                const lsd::reflect::TypeInfo* const* types = lsd::world::component_types(type_count);
                for (lsd::u32 t = 0; t < type_count; ++t) {
                    if (const void* c = world.get(e, *types[t])) {
                        const auto* bytes = static_cast<const lsd::u8*>(c);
                        item.components.emplace_back(types[t], std::vector<lsd::u8>(bytes, bytes + types[t]->size));
                    }
                }
                kept.push_back(std::move(item));
            }
            state.kept_count = 0;
            play.close_port();
            lsd::world::load_level(world, play_snapshot.data(), play_snapshot.size());  // Play changes revert, like Unreal's PIE
            lsd::u32 applied = 0;
            for (const Kept& item : kept) {  // levels reload objects at their saved ids: the same handle is the same object
                if (!world.alive(item.entity) || item.name != world.name(item.entity)) continue;  // spawned in Play: not in the level
                for (const auto& [type, bytes] : item.components) {
                    void* c = world.get(item.entity, *type);
                    if (!c) c = world.add(item.entity, *type);
                    if (c) std::memcpy(c, bytes.data(), bytes.size());  // components are plain structs
                }
                ++applied;
            }
            if (applied) {
                state.history->commit(world, "Keep Simulation Changes");
                LSD_LOG_INFO("editor", "Kept the Play changes of %u object%s", applied, applied == 1 ? "" : "s");
            }
            state.playing = false;
            lsd::editor::debug_draw::clear();
            state.mouse_captured = false;
            state.paused = state.ejected = false;
            play.stop();
            LSD_LOG_INFO("editor", "Stop");
        }
        if (game_mode && !state.playing && !state.play_requested) running = false;
        if (splash && state.playing && SDL_GetTicks() >= static_cast<lsd::u64>(state.project.splash_seconds * 1000.0f)) {
            lsd::editor::close_splash(splash);  // the game is up and the splash had its time
            splash = nullptr;
        }
        state.stop_requested = false;
        if (state.playing && play.ui_cursor()) state.mouse_captured = false;  // a menu: the cursor is free
        if (play.take_settings_changed() || state.user_settings_changed) {  // an options menu, or Settings > Engine Scalability
            state.user_settings_changed = false;
            apply_user_settings();
            lsd::reflect::save_file(user_settings, kUserSettingsPath);
        }
        SDL_SetWindowRelativeMouseMode(window, state.playing && state.mouse_captured && !state.ejected);

        if (state.playing) {
            // Devices go to the game only while it has the mouse (not when ejected or the mouse is freed).
            lsd::input::InputFrame devices;
            static bool keys[SDL_SCANCODE_COUNT];
            bool scripted_keys = false;  // input.Hold (tests, the MCP bridge) drives the game even without the mouse
            for (int k = 0; k < SDL_SCANCODE_COUNT && !scripted_keys; ++k) scripted_keys = g_held_until[k] > SDL_GetTicks();
            for (int b = 0; b < 3 && !scripted_keys; ++b) scripted_keys = g_mouse_held_until[b] > SDL_GetTicks();
            if ((state.mouse_captured && !state.ejected) || scripted_keys) {
                int count = 0;
                const bool* real = SDL_GetKeyboardState(&count);
                const lsd::u64 ticks = SDL_GetTicks();
                for (int k = 0; k < SDL_SCANCODE_COUNT; ++k) keys[k] = (k < count && real[k]) || g_held_until[k] > ticks;
                devices.keys = keys;
                devices.key_count = SDL_SCANCODE_COUNT;
                devices.mouse_dx = mouse_dx;
                devices.mouse_dy = mouse_dy;
                const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                devices.mouse_buttons = ((buttons & SDL_BUTTON_LMASK) ? 1u : 0u) | ((buttons & SDL_BUTTON_MMASK) ? 2u : 0u) | ((buttons & SDL_BUTTON_RMASK) ? 4u : 0u);
                for (int b = 0; b < 3; ++b) devices.mouse_buttons |= g_mouse_held_until[b] > ticks ? 1u << b : 0u;
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
        animations->update_outfits(world, *skinned_cache);  // clothes follow their Outfit slots
        // Skeletal meshes animate in the editor too; notifies (sounds) only in Play.
        animations->update(world, *skinned_cache, state.playing && state.paused && !state.step_requested ? 0.0f : dt,
                           state.playing && !state.paused ? &sounds : nullptr);
        animations->attach_to_sockets(world, *skinned_cache);  // weapons in hands follow this frame's pose
        effects->set_collider(state.playing ? &lsd::editor::PlaySession::segment_hit : nullptr, &play);  // particles' World collision
        effects->update(world, state.playing && state.paused && !state.step_requested ? 0.0f : dt, state.playing);
        lsd::world::Entity finished[64];  // Auto Destroy effects that are over (Play only)
        for (lsd::u32 n = effects->take_finished(finished, 64), k = 0; k < n; ++k) {
            if (world.alive(finished[k])) world.destroy(finished[k]);
        }
        // The ears: the player's camera in Play, else the editor camera.
        if (state.playing && !state.ejected) audio.set_listener(play.eye(), play.view_forward());
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
        decal_atlas->update(world);  // before the frame: the atlas may be rebuilt
        g_sprite_seconds = state.playing ? static_cast<lsd::f32>(play.clock()) : static_cast<lsd::f32>(SDL_GetTicks()) * 0.001f;
        g_registry = state.registry;
        device->begin_frame(clear_color);
        state.stat_wait_ms = timer.lap();  // the GPU (and vsync) holding the CPU back: not part of render
        device->gpu_timestamp(0);  // GPU timings (stat dump): 0 start, 1 sun shadows, 2 scene, 3 post, 4 UI
        g_gpu_mark_shadows = true;
        thumbnails.render(clear_color);
        particle_editor->render(*device, clear_color);
        lsd::editor::render_graph_preview(state, clear_color);
        lsd::editor::render_ui_editor(*device, clear_color);
        lsd::editor::render_mesh_editor(*device, clear_color);
        lsd::editor::render_prefab_editor(*device, clear_color);
        // Reflection captures: six faces from each capture's place, rendered now, read back after the frame.
        if (state.capture_reflections && reflection_capture_count == 0) {
            state.capture_reflections = false;
            const lsd::reflect::TypeInfo& rtype = lsd::reflect::type_of<lsd::world::ReflectionCapture>();
            const lsd::Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            const lsd::f32 face[4] = {0.0f, 0.0f, static_cast<lsd::f32>(lsd::render::kReflectionFace), static_cast<lsd::f32>(lsd::render::kReflectionFace)};
            const lsd::f32 black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (lsd::u32 i = 0; i < world.count(rtype) && reflection_capture_count < lsd::render::kMaxReflections; ++i) {
                const lsd::world::Entity e = world.owner(rtype, i);
                if ((!state.playing || state.ejected) && world.hidden_in_hierarchy(e)) continue;
                const lsd::Mat4 m = world.world_matrix(e);
                const lsd::Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
                lsd::Mat4 views[6];
                lsd::render::reflection_views(at, views);
                const lsd::u32 k = reflection_capture_count++;
                reflection_probes[k][0] = at.x;
                reflection_probes[k][1] = at.y;
                reflection_probes[k][2] = at.z;
                reflection_probes[k][3] = static_cast<const lsd::world::ReflectionCapture*>(world.at(rtype, i))->radius;
                for (int f = 0; f < 6; ++f) {
                    reflection_faces[k * 6 + static_cast<lsd::u32>(f)] = device->create_render_target(lsd::render::kReflectionFace, lsd::render::kReflectionFace);
                    lsd::editor::LightView face_view{at, dirs[f], lsd::radians(90.0f), 1.0f};
                    draw_world(*device, views[f], face_view, face, shadow_atlas, local_shadow_atlas, world, meshes, materials, state, reflection_faces[k * 6 + static_cast<lsd::u32>(f)], black, true);
                }
            }
            device->set_render_target({}, nullptr);
            if (reflection_capture_count == 0) {  // none in the level: nothing to reflect but the sky
                lsd::editor::set_reflection_probes(reflection_probes, 0);
                device->set_reflection_atlas({});
            }
        }
        for (lsd::u32 v = 0; v < state.viewport_count; ++v) {
            const lsd::f32* rect = state.viewport_rects[v];
            if (rect[2] < 8.0f || rect[3] < 8.0f) continue;
            const lsd::editor::EditorCamera& camera = state.viewport_count == 1 ? state.camera : state.viewport_camera(v);
            // ImGui works in window points; the swapchain is in pixels (differs on high-DPI screens).
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            device->set_viewport(rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y);
            lsd::editor::EditorCamera lens = camera;
            if (state.playing && !state.ejected) {
                lens.view = lsd::editor::ViewType::Perspective;  // the player always sees in perspective
                if (play.view_fov() > 0.0f) lens.fov_y = play.view_fov();  // a Camera component's
                if (play.view_ortho() > 0.0f) {  // an orthographic Camera (2D games)
                    lens.view = lsd::editor::ViewType::Front;
                    lens.ortho_height = play.view_ortho();
                }
            }
            lsd::Mat4 proj = lsd::editor::camera_projection(lens, rect[2] / rect[3]);
            const lsd::Mat4 view = state.playing && !state.ejected ? lsd::look_at(play.eye(), play.eye() + play.view_forward(), lsd::Vec3{0, 0, 1}) : lsd::editor::camera_view(camera);
            lsd::Mat4 view_proj = proj * view;
            state.drawn_view_proj[v] = view_proj;
            lsd::editor::LightView light_view;
            light_view.eye = state.playing && !state.ejected ? play.eye() : camera.position;
            light_view.forward = state.playing && !state.ejected ? play.view_forward() : lsd::editor::camera_forward(camera);
            light_view.fov_y = lens.fov_y;
            light_view.aspect = rect[2] / rect[3];
            const lsd::f32 pixels[4] = {rect[0] * scale.x, rect[1] * scale.y, rect[2] * scale.x, rect[3] * scale.y};
            // Post-processing (a Post Process Volume applies): the scene goes into a target the post pass reads.
            lsd::rhi::PostDraw post;
            const lsd::world::PostProcessVolume* volume = nullptr;
            const bool use_post = !state.view_mode_unlit_or_wire() && lsd::editor::world_post(world, light_view.eye, !state.playing || state.ejected, post, &volume);
            if (volume && volume->color_grading_lut[0]) {  // the volume's Color Grading LUT: a data texture
                post.color_lut = texture_cache->get(volume->color_grading_lut, false);
                post.lut_intensity = volume->lut_intensity;
            }
            if (post.contact_shadow[3] > 0.0f) {  // toward the sun: world space -> view space
                const lsd::Vec3 toward = lsd::normalize(lsd::transform_vector(view, {post.contact_shadow[0], post.contact_shadow[1], post.contact_shadow[2]}));
                post.contact_shadow[0] = toward.x;
                post.contact_shadow[1] = toward.y;
                post.contact_shadow[2] = toward.z;
            }
            post.delta_seconds = dt;  // eye adaptation eases over time
            if (use_post && lens.view == lsd::editor::ViewType::Perspective) {  // depth effects (AO, DOF, TAA) need the projection
                post.proj[0] = proj.at(0, 0);
                post.proj[1] = proj.at(1, 1);
                post.proj[2] = proj.at(2, 2);
                post.proj[3] = proj.at(2, 3);
            }
            const bool taa = post.taa && post.proj[0] != 0.0f && v < 4;
            const bool motion_blur = post.motion_blur > 0.0f && post.proj[0] != 0.0f && v < 4;
            if (taa || motion_blur) {  // last frame's clip space from this one's (without the jitter): TAA and motion blur read it
                post.taa_reset = !taa_history[v].valid;
                const lsd::Mat4 reproject = taa_history[v].view_proj * lsd::inverse(view_proj);
                std::memcpy(post.reproject, reproject.m, sizeof post.reproject);
                taa_history[v].view_proj = view_proj;
                taa_history[v].valid = true;
            }
            if (taa) {  // a sub-pixel jitter (Halton 2, 3)
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
            } else if (v < 4 && !motion_blur) {
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
            if (state.show_grid && !state.game_view && !state.playing) {
                // Side views see the floor edge-on, so they get the grid stood up on their own plane (XZ / YZ).
                const lsd::Mat4 grid_proj = lens.view == lsd::editor::ViewType::Front ? view_proj * lsd::rotation_x(lsd::radians(90.0f))
                                          : lens.view == lsd::editor::ViewType::Right ? view_proj * lsd::rotation_y(lsd::radians(90.0f))
                                                                                       : view_proj;
                device->draw_lines(grid.vertices, grid.vertex_count, grid_proj.m);
            }
            effects->draw(*device, world, view_proj, view, light_view.eye, !state.playing || state.ejected);  // after the opaque meshes
            if (scene.valid()) {
                post.scene = scene;
                for (int c = 0; c < 4; ++c) post.viewport[c] = pixels[c];
                if (v == 0) device->gpu_timestamp(2);
                device->draw_post(post);
                if (v == 0) device->gpu_timestamp(3);
            }
            if (!g_outline.empty() && (!state.playing || state.ejected)) {
                device->draw_outline(g_outline.data(), static_cast<lsd::u32>(g_outline.size()), kOutlineColor, pixels);
                g_outline.clear();
            }
            if (state.playing && !state.ejected && v == 0) {  // the game's UI over the player's view
                play.place_ui_followers(world, view_proj, pixels);
                game_ui->update(pixels);
                game_ui->render(*device);
            }
        }
        device->render_debug_ui();
        device->gpu_timestamp(4);
        state.stat_gpu_valid = device->gpu_timings(state.stat_gpu_ms);
        state.stat_render_ms = timer.lap();  // CPU side of drawing (the GPU runs alongside)
        LSD_PROFILE_FRAME();
        if ((frame == screenshot_frame || std::exchange(g_screenshot, false)) && !device->capture_frame("Saved/screenshot.bmp")) {
            LSD_LOG_WARN("editor", "-screenshot needs -rhi=opengl or -rhi=vulkan for now.");
        }
        device->end_frame();
        ++frame;
        if (reflection_capture_count) {  // the captures' faces: read back, unwrapped into the atlas, uploaded
            static std::vector<lsd::u8> atlas_pixels, face_pixels[6];
            atlas_pixels.assign(static_cast<size_t>(lsd::render::kReflectionAtlas) * lsd::render::kReflectionAtlas * 4, 0);
            lsd::Mat4 views[6];
            lsd::render::reflection_views({0.0f, 0.0f, 0.0f}, views);
            for (lsd::u32 k = 0; k < reflection_capture_count; ++k) {
                const lsd::u8* faces[6];
                bool read = true;
                for (int f = 0; f < 6; ++f) {
                    face_pixels[f].resize(static_cast<size_t>(lsd::render::kReflectionFace) * lsd::render::kReflectionFace * 4);
                    read = read && device->read_render_target(reflection_faces[k * 6 + static_cast<lsd::u32>(f)], face_pixels[f].data());
                    faces[f] = face_pixels[f].data();
                }
                if (read) lsd::render::unwrap_reflection(faces, views, k, atlas_pixels.data());
            }
            for (lsd::u32 i = 0; i < reflection_capture_count * 6; ++i) device->destroy_texture(reflection_faces[i]);
            std::vector<std::vector<lsd::u8>> levels;
            lsd::render::reflection_mips(atlas_pixels.data(), levels);
            const void* mips[lsd::render::kReflectionMips] = {atlas_pixels.data()};
            for (size_t l = 0; l < levels.size(); ++l) mips[l + 1] = levels[l].data();
            const lsd::rhi::TextureHandle atlas = device->create_texture(lsd::render::kReflectionAtlas, lsd::render::kReflectionAtlas, lsd::render::kReflectionMips,
                                                                         lsd::rhi::TextureFormat::RGBA8, mips);
            device->set_reflection_atlas(atlas);
            if (reflection_atlas.valid()) device->destroy_texture(reflection_atlas);  // shortcut: freed at once; defer a frame if a backend complains
            reflection_atlas = atlas;
            lsd::editor::set_reflection_probes(reflection_probes, reflection_capture_count);
            LSD_LOG_INFO("editor", "Reflection captures: %u captured", reflection_capture_count);
            reflection_capture_count = 0;
        }
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
    lsd::world::set_prefab_resolver(nullptr, nullptr);  // it points at the registry
    lsd::editor::shutdown_graph_editor();
    lsd::log::set_sink(nullptr, nullptr);
    std::snprintf(state.settings.last_level, sizeof state.settings.last_level, "%s", state.level_path);
    if (!game_mode) lsd::reflect::save_file(state.settings, kSettingsPath);
    play.stop();
    stop_play_clients();
    close_play_window(true);
    lsd::editor::game_module::shutdown();
    lsd::editor::game_module::unload();  // after Play stopped: no behaviour runs its code any more
    audio.shutdown();
    SDL_SetWindowRelativeMouseMode(window, false);
    if (gamepad) SDL_CloseGamepad(gamepad);
    lsd::render::destroy_lines(*device, grid);
    for (const lsd::rhi::TextureHandle t : scene_targets.target) {
        if (t.valid()) device->destroy_texture(t);
    }
    lsd::editor::shutdown_mesh_editor();
    lsd::editor::shutdown_prefab_editor();
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
    g_decals = nullptr;
    delete decal_atlas;
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
    lsd::editor::close_splash(splash);  // the game ended before it was up
    SDL_DestroyWindow(window);
    SDL_Quit();
    lsd::log::open_file(nullptr);  // last: a failure while shutting down still reaches the log file
    return exit_code;
}
