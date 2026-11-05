#include "editor_modes.h"
#include "tips.h"

#include "material_cache.h"
#include "mesh_cache.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/guid.h"
#include "lsd/foundation/allocator.h"
#include "lsd/particles/particles.h"
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"
#include "lsd/world/timeline.h"
#include "lsd/world/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lsd::editor {

namespace {

using world::Entity;

struct ModeInfo {
    EditorMode mode;
    const char* name;
    const char* tooltip;
};

constexpr ModeInfo kModes[] = {
    {EditorMode::Select, "Select", "Click to select; the gizmo moves, turns and scales (the default)"},
    {EditorMode::Modeling, "3D Modeling", "Blockout shapes: place, size and texture them (Unreal's Modeling Mode basics)"},
    {EditorMode::Foliage, "Foliage", "Paint meshes onto surfaces with a brush (Unreal's Foliage Mode)"},
    {EditorMode::Landscape, "Landscape", "Make a terrain and sculpt it (Unreal's Landscape Mode)"},
    {EditorMode::Animation, "Animation Sequence", "Timelines: moves, properties, events, sounds and camera cuts over time (Sequencer)"},
    {EditorMode::Pcg, "PCG", "Volumes that scatter meshes over surfaces by rules (Unreal's PCG)"},
    {EditorMode::Vfx, "VFX", "Particle systems: make, edit and place them (Niagara)"},
    {EditorMode::Paper2D, "2D", "Sprites, flipbooks and tile maps for 2D games (Unreal's Paper2D)"},
};

constexpr f32 kPi = 3.14159265f;
constexpr u32 kGrid = world::Landscape::kResolution;

u64 fnv(const void* data, usize bytes, u64 h = 1469598103934665603ull) {
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

Entity find_named(const world::World& w, const char* name) {
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (e.valid() && std::strcmp(w.name(e), name) == 0) return e;
    }
    return {};
}

bool project(const Mat4& view_proj, Vec3 p, ImVec2 pos, ImVec2 size, ImVec2& out) {
    const f32 x = view_proj.at(0, 0) * p.x + view_proj.at(0, 1) * p.y + view_proj.at(0, 2) * p.z + view_proj.at(0, 3);
    const f32 y = view_proj.at(1, 0) * p.x + view_proj.at(1, 1) * p.y + view_proj.at(1, 2) * p.z + view_proj.at(1, 3);
    const f32 w = view_proj.at(3, 0) * p.x + view_proj.at(3, 1) * p.y + view_proj.at(3, 2) * p.z + view_proj.at(3, 3);
    if (w < 0.01f) return false;
    out = ImVec2(pos.x + (x / w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - y / w * 0.5f) * size.y);
    return true;
}

void hint(const char* text) {  // greyed help that wraps at the panel's edge
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Where the viewport's centre looks: new things go there.
Vec3 view_target(EditorState& s) {
    const Vec3 dir = camera_forward(s.camera);
    f32 t;
    if (surface_ray(s, s.camera.position, dir, t)) return s.camera.position + dir * t;
    if (dir.z < -1e-4f) return s.camera.position + dir * (-s.camera.position.z / dir.z);
    return s.camera.position + dir * s.camera.pivot_distance;
}

// A mesh picker: the basic shapes, then every mesh asset. `key` is a StaticMesh-style mesh key.
bool mesh_combo(EditorState& s, const char* label, char* key, usize capacity) {
    const char* shown = key;
    asset::Guid guid;
    if (asset::parse_guid(key, guid)) {
        if (const asset::AssetRecord* r = s.registry->find(guid)) shown = r->name;
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, shown)) {
        for (const char* shape : {"Cube", "Sphere", "Plane"}) {
            if (ImGui::Selectable(shape, std::strcmp(key, shape) == 0)) {
                std::snprintf(key, capacity, "%s", shape);
                changed = true;
            }
        }
        for (usize i = 0; i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Mesh) continue;
            char text[33];
            asset::to_string(r.guid, text);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(r.name, std::strcmp(key, text) == 0)) {
                std::snprintf(key, capacity, "%s", text);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Asset rows of one data type, each opening its editor; `place` adds a Place button.
void asset_list(EditorState& s, const char* data_type, const char* empty, bool place) {
    u32 shown = 0;
    for (usize i = 0; i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        if (!asset_matches(r, data_type)) continue;
        ++shown;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(r.name, false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(place ? ImGui::GetContentRegionAvail().x - 60.0f : 0.0f, 0.0f)) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            open_asset(s, r);
        }
        ImGui::SetItemTooltip("Double-click to edit: Content/%s", r.path);
        if (place) {
            ImGui::SameLine();
            if (with_tip(ImGui::SmallButton("Place"), "Puts this in the level in front of the camera (or drag it into the viewport)")) {
                const Entity e = place_object(s, r.name, nullptr, "ParticleEffect", view_target(s));
                asset::to_string(r.guid, s.world->get<world::ParticleEffect>(e)->system);
            }
            ImGui::SetItemTooltip("Add a Particle Effect playing it where the viewport looks");
        }
        ImGui::PopID();
    }
    if (!shown) hint(empty);
}

// ---- 3D Modeling ----

bool g_box_uv = true;  // new shapes get box-projected UVs (textures do not stretch)

void draw_modeling(EditorState& s) {
    ImGui::SeparatorText("Shapes");
    for (const char* shape : {"Cube", "Sphere", "Plane"}) {
        if (ImGui::Button(shape, ImVec2(80, 0))) {
            const Entity e = place_object(s, shape, shape, nullptr, view_target(s));
            s.world->get<world::StaticMesh>(e)->box_uv = g_box_uv;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Checkbox("Box UV on new shapes", &g_box_uv);
    ImGui::SetItemTooltip("One texture tile per metre on every face, however the shape is sized");

    ImGui::SeparatorText("Selected");
    world::Transform* t = s.world->alive(s.selected) ? s.world->get<world::Transform>(s.selected) : nullptr;
    world::StaticMesh* mesh = s.world->alive(s.selected) ? s.world->get<world::StaticMesh>(s.selected) : nullptr;
    if (!t || !mesh) {
        hint("Select a mesh to size it here.");
        return;
    }
    ImGui::Text("%s", s.world->name(s.selected));
    // The basic shapes are 1 m, so their scale is their size in metres.
    if (ImGui::DragFloat3("Size (m)", &t->scale.x, 0.05f, 0.01f, 1000.0f, "%.2f")) mark_edited(s, "Resize");
    if (with_tip(ImGui::Button("Snap size to 0.5 m"), "Rounds the selected shape's size to half-metre steps (tidy blockouts)")) {
        for (f32* v : {&t->scale.x, &t->scale.y, &t->scale.z}) *v = std::fmax(0.5f, std::round(*v * 2.0f) * 0.5f);
        mark_edited(s, "Resize");
    }
    if (with_tip(ImGui::Checkbox("Box UV", &mesh->box_uv), "Projects the texture from the box's sides: one tile per metre at any size, no stretching")) mark_edited(s, "Box UV");
    if (with_tip(ImGui::Checkbox("Collision", &mesh->collision), "The player and physics bump into it")) mark_edited(s, "Collision");
    hint("W / E / R: move, turn, scale with the gizmo.");
    hint("Extrude, bevel and booleans are not in yet.");
}

// ---- Foliage ----

struct FoliageBrush {
    char mesh[40] = "Sphere";
    f32 radius = 3.0f;
    f32 density = 1.0f;  // copies per square metre
    f32 scale_min = 0.8f, scale_max = 1.2f;
    bool random_yaw = true;
    LinearColor color = LinearColor::hex(0x4F8A3C);
};
FoliageBrush g_foliage;
std::mt19937 g_rng{12345};

f32 random01() { return std::uniform_real_distribution<f32>(0.0f, 1.0f)(g_rng); }

bool is_foliage(const world::World& w, Entity e, const char* mesh) {
    const auto* ism = w.get<world::InstancedStaticMesh>(e);
    return ism && std::strncmp(w.name(e), "Foliage", 7) == 0 && std::strcmp(ism->mesh, mesh) == 0;
}

void foliage_stamp(EditorState& s, Vec3 center, bool erase) {
    world::World& w = *s.world;
    const FoliageBrush& b = g_foliage;
    std::vector<Entity> layers;  // shortcut: std in editor-only code
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (e.valid() && is_foliage(w, e, b.mesh)) layers.push_back(e);
    }
    bool changed = false;
    if (erase) {
        for (const Entity e : layers) {
            auto* ism = w.get<world::InstancedStaticMesh>(e);
            const Mat4 m = w.world_matrix(e);
            for (u32 k = ism->instances.count; k-- > 0;) {
                const Vec3 p = transform_point(m, ism->instances[k].position);
                if ((p.x - center.x) * (p.x - center.x) + (p.y - center.y) * (p.y - center.y) < b.radius * b.radius) {
                    ism->instances.remove(k);
                    changed = true;
                }
            }
        }
        if (changed) mark_edited(s, "Erase Foliage");
        return;
    }
    const f32 spacing = 1.0f / std::sqrt(std::fmax(b.density, 0.001f)) * 0.7f;
    const u32 tries = static_cast<u32>(std::fmin(24.0f, std::fmax(1.0f, b.density * kPi * b.radius * b.radius * 0.08f)));
    for (u32 i = 0; i < tries; ++i) {
        const f32 r = b.radius * std::sqrt(random01()), a = random01() * 2.0f * kPi;
        const Vec3 top{center.x + r * std::cos(a), center.y + r * std::sin(a), center.z + b.radius + 20.0f};
        f32 t;
        if (!surface_ray(s, top, {0, 0, -1}, t)) continue;
        const Vec3 p = top + Vec3{0, 0, -t};
        bool crowded = false;
        for (const Entity e : layers) {
            const auto* ism = w.get<world::InstancedStaticMesh>(e);
            const Mat4 m = w.world_matrix(e);
            for (u32 k = 0; k < ism->instances.count && !crowded; ++k) {
                const Vec3 q = transform_point(m, ism->instances[k].position);
                crowded = length(q - p) < spacing;
            }
        }
        if (crowded) continue;
        Entity layer;
        for (const Entity e : layers) {
            if (w.get<world::InstancedStaticMesh>(e)->instances.count < decltype(world::InstancedStaticMesh::instances)::capacity()) layer = e;
        }
        if (!layer.valid()) {  // a new layer object for this mesh (1024 copies each)
            char name[64];
            w.unique_name("Foliage", name, sizeof name);
            layer = w.create(name);
            auto* ism = w.add<world::InstancedStaticMesh>(layer);
            std::snprintf(ism->mesh, sizeof ism->mesh, "%s", b.mesh);
            ism->color = b.color;
            layers.push_back(layer);
        }
        world::MeshInstance in;
        in.position = transform_point(affine_inverse(w.world_matrix(layer)), p);
        in.rotation = {0, 0, b.random_yaw ? random01() * 360.0f : 0.0f};
        const f32 k = b.scale_min + (b.scale_max - b.scale_min) * random01();
        in.scale = {k, k, k};
        w.get<world::InstancedStaticMesh>(layer)->instances.push_back(in);
        changed = true;
    }
    if (changed) mark_edited(s, "Paint Foliage");
}

void draw_foliage(EditorState& s) {
    ImGui::SeparatorText("Paint");
    mesh_combo(s, "Mesh", g_foliage.mesh, sizeof g_foliage.mesh);
    with_tip(ImGui::ColorEdit3("Tint", &g_foliage.color.r), "Colour of the painted copies");
    with_tip(ImGui::DragFloat("Brush Size (m)", &g_foliage.radius, 0.1f, 0.2f, 100.0f, "%.1f"), "Radius of the brush circle in the viewport");
    with_tip(ImGui::DragFloat("Density (per m2)", &g_foliage.density, 0.02f, 0.01f, 20.0f, "%.2f"), "How many copies a stroke puts in each square metre");
    ImGui::DragFloatRange2("Scale", &g_foliage.scale_min, &g_foliage.scale_max, 0.01f, 0.05f, 10.0f, "%.2f");
    with_tip(ImGui::Checkbox("Random turn", &g_foliage.random_yaw), "Each copy gets a random turn so they do not all face the same way");
    hint("LMB drag in the viewport paints; Shift+LMB erases.");
    hint("Copies go to Foliage objects (Instanced Static Meshes).");
    u32 total = 0;
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (e.valid() && is_foliage(*s.world, e, g_foliage.mesh)) total += s.world->get<world::InstancedStaticMesh>(e)->instances.count;
    }
    ImGui::Text("%u copies of this mesh", total);
}

// ---- Landscape ----

struct SculptBrush {
    int tool = 0;  // 0 Sculpt, 1 Smooth, 2 Flatten
    f32 radius = 5.0f;
    f32 strength = 1.0f;
};
SculptBrush g_sculpt;
f32 g_new_size = 64.0f;
std::unordered_map<std::string, u64> g_landscape_hash;  // mesh key -> heights it was built from

void build_landscape_mesh(const world::Landscape& l, render::MeshData& data) {
    data = render::allocate_mesh(default_allocator(), kGrid * kGrid, (kGrid - 1) * (kGrid - 1) * 6);
    const f32 step = l.size / static_cast<f32>(kGrid - 1), half = l.size * 0.5f;
    auto h = [&](i32 i, i32 j) {
        i = i < 0 ? 0 : i >= static_cast<i32>(kGrid) ? static_cast<i32>(kGrid) - 1 : i;
        j = j < 0 ? 0 : j >= static_cast<i32>(kGrid) ? static_cast<i32>(kGrid) - 1 : j;
        const u32 k = static_cast<u32>(j) * kGrid + static_cast<u32>(i);
        return k < l.heights.count ? l.heights[k] : 0.0f;
    };
    const f32 tile = l.uv_tile > 0.01f ? l.uv_tile : 0.01f;
    for (i32 j = 0; j < static_cast<i32>(kGrid); ++j) {
        for (i32 i = 0; i < static_cast<i32>(kGrid); ++i) {
            rhi::MeshVertex& v = data.vertices[static_cast<u32>(j) * kGrid + static_cast<u32>(i)];
            const f32 x = -half + static_cast<f32>(i) * step, y = -half + static_cast<f32>(j) * step;
            const Vec3 n = normalize(Vec3{(h(i - 1, j) - h(i + 1, j)) / (2.0f * step), (h(i, j - 1) - h(i, j + 1)) / (2.0f * step), 1.0f});
            v = {{x, y, h(i, j)}, {n.x, n.y, n.z}, {x / tile, -y / tile}};
        }
    }
    u32 k = 0;
    for (u32 j = 0; j + 1 < kGrid; ++j) {
        for (u32 i = 0; i + 1 < kGrid; ++i) {
            const u32 a = j * kGrid + i, b = a + 1, c = a + kGrid, d = c + 1;
            for (const u32 index : {a, b, d, a, d, c}) data.indices[k++] = index;  // counter-clockwise from above
        }
    }
}

std::vector<Entity> landscapes(const world::World& w) {
    std::vector<Entity> out;
    const reflect::TypeInfo& type = reflect::type_of<world::Landscape>();
    for (u32 i = 0; i < w.count(type); ++i) out.push_back(w.owner(type, i));
    return out;
}

void sculpt(EditorState& s, Vec3 center, f32 dt, f32 flatten_z) {
    const bool lower = ImGui::GetIO().KeyShift;
    bool changed = false;
    for (const Entity e : landscapes(*s.world)) {
        auto* l = s.world->get<world::Landscape>(e);
        if (l->heights.count < kGrid * kGrid) continue;  // filled by update_landscapes on its first frame
        const Mat4 m = s.world->world_matrix(e), inv = affine_inverse(m);
        const Vec3 c = transform_point(inv, center);
        const f32 sx = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
        const f32 radius = g_sculpt.radius / (sx > 1e-4f ? sx : 1.0f);
        const f32 target = transform_point(inv, {center.x, center.y, flatten_z}).z;
        const f32 step = l->size / static_cast<f32>(kGrid - 1), half = l->size * 0.5f;
        static std::vector<f32> before;
        before.assign(l->heights.items, l->heights.items + kGrid * kGrid);
        const i32 i0 = static_cast<i32>(std::floor((c.x - radius + half) / step)), i1 = static_cast<i32>(std::ceil((c.x + radius + half) / step));
        const i32 j0 = static_cast<i32>(std::floor((c.y - radius + half) / step)), j1 = static_cast<i32>(std::ceil((c.y + radius + half) / step));
        for (i32 j = j0 < 0 ? 0 : j0; j <= j1 && j < static_cast<i32>(kGrid); ++j) {
            for (i32 i = i0 < 0 ? 0 : i0; i <= i1 && i < static_cast<i32>(kGrid); ++i) {
                const f32 dx = -half + static_cast<f32>(i) * step - c.x, dy = -half + static_cast<f32>(j) * step - c.y;
                const f32 d = std::sqrt(dx * dx + dy * dy);
                if (d > radius) continue;
                const f32 weight = 0.5f * (1.0f + std::cos(kPi * d / radius));  // soft edge
                const f32 amount = g_sculpt.strength * dt * weight;
                const u32 k = static_cast<u32>(j) * kGrid + static_cast<u32>(i);
                f32& h = l->heights[k];
                if (g_sculpt.tool == 0) {
                    h += amount * 2.0f * (lower ? -1.0f : 1.0f);  // 2 m a second at full strength
                } else {
                    f32 goal = target;
                    if (g_sculpt.tool == 1) {  // the average of the four neighbours
                        auto at = [&](i32 a, i32 b) { return before[static_cast<u32>(b < 0 ? 0 : b >= static_cast<i32>(kGrid) ? kGrid - 1 : static_cast<u32>(b)) * kGrid + static_cast<u32>(a < 0 ? 0 : a >= static_cast<i32>(kGrid) ? kGrid - 1 : static_cast<u32>(a))]; };
                        goal = (at(i - 1, j) + at(i + 1, j) + at(i, j - 1) + at(i, j + 1)) * 0.25f;
                    }
                    h += (goal - h) * std::fmin(1.0f, amount * 6.0f);
                }
                changed = true;
            }
        }
    }
    if (changed) mark_edited(s, "Sculpt Landscape");
}

void draw_landscape(EditorState& s) {
    const std::vector<Entity> all = landscapes(*s.world);
    ImGui::SeparatorText("New Landscape");
    with_tip(ImGui::DragFloat("Size (m)", &g_new_size, 1.0f, 4.0f, 2000.0f, "%.0f"), "Width and depth of the new landscape");
    if (with_tip(ImGui::Button("Create Landscape"), "Adds a flat landscape you can sculpt (Unreal's Landscape > Create)")) {
        const Vec3 at = view_target(s);
        const Entity e = place_object(s, "Landscape", nullptr, "Landscape", {at.x, at.y, 0.0f});
        s.world->get<world::Landscape>(e)->size = g_new_size;
    }
    ImGui::SetItemTooltip("A flat square of terrain where the viewport looks, %u x %u heights", kGrid, kGrid);
    ImGui::SeparatorText("Sculpt");
    if (all.empty()) {
        hint("Create a landscape first.");
        return;
    }
    ImGui::RadioButton("Sculpt", &g_sculpt.tool, 0);
    ImGui::SetItemTooltip("LMB raises, Shift+LMB lowers");
    ImGui::SameLine();
    with_tip(ImGui::RadioButton("Smooth", &g_sculpt.tool, 1), "Brush that evens out bumps");
    ImGui::SameLine();
    ImGui::RadioButton("Flatten", &g_sculpt.tool, 2);
    ImGui::SetItemTooltip("Levels toward the height where the stroke started");
    with_tip(ImGui::DragFloat("Brush Size (m)", &g_sculpt.radius, 0.1f, 0.5f, 200.0f, "%.1f"), "Radius of the brush circle in the viewport");
    with_tip(ImGui::DragFloat("Strength", &g_sculpt.strength, 0.01f, 0.05f, 5.0f, "%.2f"), "How much each stroke raises, lowers or smooths");
    hint("LMB drag on the terrain sculpts (Shift lowers).");
    if (with_tip(ImGui::Button("Flatten All"), "Sets every height back to flat (undoable)")) {
        for (const Entity e : all) {
            auto* l = s.world->get<world::Landscape>(e);
            for (u32 k = 0; k < l->heights.count; ++k) l->heights[k] = 0.0f;
        }
        mark_edited(s, "Flatten Landscape");
    }
    hint("Material and collision: the Static Mesh in Details.");
}

// ---- PCG ----

std::unordered_map<std::string, u64> g_pcg_hash;  // volume name -> rules and box it was built from

u64 pcg_hash(const world::World& w, Entity e) {
    world::PcgVolume rules = *w.get<world::PcgVolume>(e);
    rules.output[0] = '\0';
    const Mat4 m = w.world_matrix(e);
    return fnv(m.m, sizeof m.m, fnv(&rules, sizeof rules));
}

void draw_pcg(EditorState& s) {
    ImGui::SeparatorText("Volumes");
    if (with_tip(ImGui::Button("Add PCG Volume"), "Adds a PCG Volume in front of the camera; it scatters a mesh on the surfaces inside its box")) {
        const Entity e = place_object(s, "PCG Volume", nullptr, "PcgVolume", view_target(s));
        s.world->get<world::Transform>(e)->scale = {12, 12, 4};
        generate_pcg(s, e);
    }
    ImGui::SetItemTooltip("A box where the viewport looks; it scatters copies on the surfaces inside it");
    const reflect::TypeInfo& type = reflect::type_of<world::PcgVolume>();
    if (!s.world->count(type)) hint("No PCG volumes in this level.");
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(s.world->name(e), s.selected == e, 0, ImVec2(ImGui::GetContentRegionAvail().x - 80.0f, 0.0f))) {
            s.selected = e;
            s.selection[0] = e;
            s.selection_count = 1;
        }
        ImGui::SameLine();
        if (with_tip(ImGui::SmallButton("Generate"), "Scatters again with the volume's current settings")) {
            generate_pcg(s, e);
            mark_edited(s, "Generate PCG");
        }
        ImGui::PopID();
    }
    hint("Rules (mesh, density, seed, slope, scale) are in Details.");
    hint("Move, size or change a volume and it rebuilds itself.");
}

// ---- Animation Sequence, VFX ----

void draw_animation(EditorState& s) {
    ImGui::SeparatorText("Timelines");
    if (with_tip(ImGui::Button("New Timeline"), "Creates a Timeline asset (Unreal's Level Sequence) in the open Content folder")) new_data_asset(s, reflect::type_of<world::Timeline>());
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Animation workspace"), "Switches to the Animation workspace (timeline and animation editors)")) std::snprintf(s.workspace_request, sizeof s.workspace_request, "Animation");
    asset_list(s, "Timeline", "No Timelines yet: New Timeline makes one in the open Content Browser folder.", false);
    hint("Play one from a script or visual script: Play Timeline.");
}

void draw_vfx(EditorState& s) {
    ImGui::SeparatorText("Particle Systems");
    if (with_tip(ImGui::Button("New Particle System"), "Creates a Particle System asset (Unreal's Niagara System) in the open Content folder")) new_data_asset(s, reflect::type_of<particles::ParticleSystem>());
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Effects workspace"), "Switches to the Effects workspace (particle and sound cue editors)")) std::snprintf(s.workspace_request, sizeof s.workspace_request, "Effects");
    asset_list(s, "ParticleSystem", "No particle systems yet: New Particle System makes one.", true);
}

// ---- 2D (Paper2D) ----

char g_2d_texture[33] = "";  // the image new sprites / tile maps use
u32 g_tile = 0;              // the palette's chosen tile (tileset cell)

// The tile map being painted: the selected one, else the first.
world::TileMap* paint_map(EditorState& s, Entity& owner) {
    if (s.selected.valid()) {
        if (auto* map = s.world->get<world::TileMap>(s.selected)) {
            owner = s.selected;
            return map;
        }
    }
    const reflect::TypeInfo& type = reflect::type_of<world::TileMap>();
    if (!s.world->count(type)) return nullptr;
    owner = s.world->owner(type, 0);
    return static_cast<world::TileMap*>(s.world->at(type, 0));
}

void texture_picker(EditorState& s) {
    const asset::AssetRecord* chosen = nullptr;
    for (usize i = 0; s.registry && i < s.registry->count(); ++i) {
        const asset::AssetRecord& r = s.registry->get(i);
        char guid[33];
        asset::to_string(r.guid, guid);
        if (r.type == asset::AssetType::Texture && std::strcmp(guid, g_2d_texture) == 0) chosen = &r;
    }
    if (ImGui::BeginCombo("Image", chosen ? chosen->name : "(pick a texture)")) {
        for (usize i = 0; s.registry && i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Texture) continue;
            char guid[33];
            asset::to_string(r.guid, guid);
            if (ImGui::Selectable(r.name, std::strcmp(guid, g_2d_texture) == 0)) std::snprintf(g_2d_texture, sizeof g_2d_texture, "%s", guid);
        }
        ImGui::EndCombo();
    }
}

void draw_2d(EditorState& s) {
    if (ImGui::Button("2D View")) set_view(s, ViewType::Front);
    ImGui::SetItemTooltip("Look along +Y without perspective, like a side-on 2D game (H)");
    ImGui::SeparatorText("New");
    texture_picker(s);
    if (with_tip(ImGui::Button("Sprite"), "Places a Sprite: a flat image facing the 2D camera (pick its Texture in Details)")) {
        const Vec3 at = view_target(s);
        const Entity e = place_object(s, "Sprite", nullptr, "Sprite", {at.x, 0.0f, at.z});
        std::snprintf(s.world->get<world::Sprite>(e)->texture, sizeof(world::Sprite::texture), "%s", g_2d_texture);
    }
    ImGui::SetItemTooltip("The image on a 1 m quad; a sprite sheet: set Columns / Rows and Frame Count in Details");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Tile Map"), "Places a Tile Map to paint with the tile palette below")) {
        const Vec3 at = view_target(s);
        const Entity e = place_object(s, "Tile Map", nullptr, "TileMap", {at.x, 0.0f, at.z});
        auto* map = s.world->get<world::TileMap>(e);
        std::snprintf(map->tileset, sizeof map->tileset, "%s", g_2d_texture);
        map->tiles.count = 0;
        for (u32 i = 0; i < map->width * map->height && i < map->tiles.capacity(); ++i) map->tiles.push_back(0);
    }
    ImGui::SetItemTooltip("An empty grid using the image as its tileset (set its Columns / Rows in Details), then paint below");
    Entity owner;
    world::TileMap* map = paint_map(s, owner);
    ImGui::SeparatorText("Paint Tiles");
    if (!map) {
        hint("Make a Tile Map first.");
        return;
    }
    ImGui::Text("Painting %s", s.world->name(owner));
    const rhi::TextureHandle tileset = map->tileset[0] && s.textures ? s.textures->get(map->tileset, true) : rhi::TextureHandle{};
    const u64 image = tileset.valid() && s.device ? s.device->debug_ui_texture(tileset) : 0;
    if (!image) {
        hint("Pick the map's Tileset texture in Details.");
        return;
    }
    // The palette: the tileset with its grid; click a tile to paint with it.
    const f32 width = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(width, width * static_cast<f32>(map->tileset_rows) / static_cast<f32>(map->tileset_columns ? map->tileset_columns : 1));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(image)), size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 cw = size.x / static_cast<f32>(map->tileset_columns), ch = size.y / static_cast<f32>(map->tileset_rows);
    for (u32 c = 1; c < map->tileset_columns; ++c) {
        const f32 x = at.x + cw * static_cast<f32>(c);
        dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + size.y), IM_COL32(0, 0, 0, 90));
    }
    for (u32 r = 1; r < map->tileset_rows; ++r) {
        const f32 y = at.y + ch * static_cast<f32>(r);
        dl->AddLine(ImVec2(at.x, y), ImVec2(at.x + size.x, y), IM_COL32(0, 0, 0, 90));
    }
    if (ImGui::IsItemClicked()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        g_tile = static_cast<u32>((m.y - at.y) / ch) * map->tileset_columns + static_cast<u32>((m.x - at.x) / cw);
    }
    const f32 col = static_cast<f32>(g_tile % map->tileset_columns), row = static_cast<f32>(g_tile / map->tileset_columns);
    dl->AddRect(ImVec2(at.x + cw * col, at.y + ch * row), ImVec2(at.x + cw * (col + 1.0f), at.y + ch * (row + 1.0f)), IM_COL32(255, 230, 30, 255), 0.0f, 0, 2.0f);
    hint("LMB paints the chosen tile on the map in the viewport, Shift+LMB erases. Use the 2D View.");
}

// The viewport brush in 2D: the tile under the mouse on the painted map's plane.
bool tile_brush(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, bool hovered) {
    static bool stroke = false;
    const ImGuiIO& io = ImGui::GetIO();
    Entity owner;
    world::TileMap* map = paint_map(s, owner);
    if (!map || io.KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (!hovered && !stroke)) {
        stroke = false;
        return false;
    }
    Vec3 o, d;
    viewport_ray(s.camera, pos, size, io.MousePos, o, d);
    const Mat4 m = s.world->world_matrix(owner);
    const Mat4 inv = affine_inverse(m);
    const Vec3 lo = transform_point(inv, o), ld = transform_vector(inv, d);
    if (std::fabs(ld.y) < 1e-6f) return hovered;
    const Vec3 p = lo + ld * (-lo.y / ld.y);  // where the ray meets the map's plane
    const i32 tx = static_cast<i32>(std::floor(p.x / map->tile_size)), tz = static_cast<i32>(std::floor(p.z / map->tile_size));
    if (tx < 0 || tz < 0 || tx >= static_cast<i32>(map->width) || tz >= static_cast<i32>(map->height)) {
        stroke = stroke && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        return stroke;
    }
    // The hovered tile's outline.
    const f32 ts = map->tile_size;
    const Vec3 corners[4] = {{tx * ts, 0, tz * ts}, {(tx + 1) * ts, 0, tz * ts}, {(tx + 1) * ts, 0, (tz + 1) * ts}, {tx * ts, 0, (tz + 1) * ts}};
    ImVec2 outline[4];
    int points = 0;
    for (const Vec3& c : corners) points += project(view_proj, transform_point(m, c), pos, size, outline[points]) ? 1 : 0;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (points == 4) dl->AddPolyline(outline, 4, io.KeyShift ? IM_COL32(255, 110, 80, 255) : IM_COL32(120, 230, 255, 255), ImDrawFlags_Closed, 2.0f);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) stroke = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) stroke = false;
    if (stroke) {
        const u32 cells = map->width * map->height < map->tiles.capacity() ? map->width * map->height : map->tiles.capacity();
        while (map->tiles.count < cells) map->tiles.push_back(0);
        const u32 index = static_cast<u32>(tz) * map->width + static_cast<u32>(tx);
        const i32 value = io.KeyShift ? 0 : static_cast<i32>(g_tile) + 1;
        if (index < map->tiles.count && map->tiles[index] != value) {
            map->tiles[index] = value;
            mark_edited(s, "Paint Tiles");
        }
    }
    return true;
}

} // namespace

const char* mode_name(EditorMode mode) {
    for (const ModeInfo& m : kModes) {
        if (m.mode == mode) return m.name;
    }
    return "";
}

void draw_mode_menu(EditorState& s) {
    hint("Mode");
    for (const ModeInfo& m : kModes) {
        if (ImGui::MenuItem(m.name, nullptr, s.mode == m.mode)) s.mode = m.mode;
        ImGui::SetItemTooltip("%s", m.tooltip);
    }
}

void draw_mode_panel(EditorState& s) {
    if (s.mode == EditorMode::Select) return;
    const ModeInfo* info = &kModes[0];
    for (const ModeInfo& m : kModes) {
        if (m.mode == s.mode) info = &m;
    }
    char title[64];
    std::snprintf(title, sizeof title, "%s Mode###ModePanel", info->name);
    if (s.viewport_count) {  // first time: along the viewport's left edge, like Unreal's mode toolbox
        const f32* r = s.viewport_rects[0];
        ImGui::SetNextWindowPos(ImVec2(r[0] + 8.0f, r[1] + 48.0f), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(330, 420), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin(title, &open)) {
        switch (s.mode) {
            case EditorMode::Modeling: draw_modeling(s); break;
            case EditorMode::Foliage: draw_foliage(s); break;
            case EditorMode::Landscape: draw_landscape(s); break;
            case EditorMode::Animation: draw_animation(s); break;
            case EditorMode::Pcg: draw_pcg(s); break;
            case EditorMode::Vfx: draw_vfx(s); break;
            case EditorMode::Paper2D: draw_2d(s); break;
            case EditorMode::Select: break;
        }
    }
    ImGui::End();
    if (!open) s.mode = EditorMode::Select;
}

bool mode_brush(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, bool hovered) {
    if (s.mode == EditorMode::Paper2D) return tile_brush(s, view_proj, pos, size, hovered);
    static bool stroke = false;
    static f32 flatten_z = 0.0f, since_stamp = 0.0f;
    const ImGuiIO& io = ImGui::GetIO();
    const bool painting = s.mode == EditorMode::Foliage || (s.mode == EditorMode::Landscape && !landscapes(*s.world).empty());
    if (!painting || io.KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (!hovered && !stroke)) {
        stroke = false;
        return false;
    }
    Vec3 o, d;
    viewport_ray(s.camera, pos, size, io.MousePos, o, d);
    f32 t;
    Vec3 p;
    if (surface_ray(s, o, d, t)) p = o + d * t;
    else if (d.z < -1e-4f) p = o + d * (-o.z / d.z);
    else return hovered;
    const f32 radius = s.mode == EditorMode::Foliage ? g_foliage.radius : g_sculpt.radius;
    ImDrawList* dl = ImGui::GetForegroundDrawList();  // the brush ring, lying on the surface height
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    ImVec2 ring[48];
    int points = 0;
    for (int k = 0; k < 48; ++k) {
        const f32 a = static_cast<f32>(k) / 48.0f * 2.0f * kPi;
        if (project(view_proj, p + Vec3{radius * std::cos(a), radius * std::sin(a), 0.0f}, pos, size, ring[points])) ++points;
    }
    const ImU32 color = io.KeyShift ? IM_COL32(255, 110, 80, 255) : IM_COL32(120, 230, 255, 255);
    if (points > 2) dl->AddPolyline(ring, points, color, ImDrawFlags_Closed, 2.0f);
    dl->PopClipRect();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        stroke = true;
        flatten_z = p.z;
        since_stamp = 1.0f;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) stroke = false;
    if (stroke) {
        if (s.mode == EditorMode::Foliage) {
            since_stamp += io.DeltaTime;
            if (since_stamp >= 0.06f) {  // about 16 stamps a second, however fast the frames
                since_stamp = 0.0f;
                foliage_stamp(s, p, io.KeyShift);
            }
        } else {
            sculpt(s, p, io.DeltaTime < 0.1f ? io.DeltaTime : 0.1f, flatten_z);
        }
    }
    return true;
}

void update_landscapes(world::World& w, MeshCache& meshes) {
    std::unordered_set<std::string> keys;  // a duplicated landscape gets a key of its own
    for (const Entity e : landscapes(w)) {
        auto* l = w.get<world::Landscape>(e);
        while (l->heights.count < kGrid * kGrid) l->heights.push_back(0.0f);
        world::StaticMesh* mesh = w.get<world::StaticMesh>(e);
        if (!mesh) {
            mesh = w.add<world::StaticMesh>(e);
            mesh->collision_shape = 1;  // the triangles: walk on the slopes
            mesh->color = LinearColor::hex(0x6E8A55);
            mesh->mesh[0] = '\0';
            l = w.get<world::Landscape>(e);
        }
        if (std::strncmp(mesh->mesh, "Landscape:", 10) != 0 || keys.count(mesh->mesh)) {
            static std::mt19937 ids{std::random_device{}()};
            std::snprintf(mesh->mesh, sizeof mesh->mesh, "Landscape:%08x", static_cast<unsigned>(ids()));
        }
        keys.insert(mesh->mesh);
        const u64 hash = fnv(&l->size, sizeof l->size, fnv(&l->uv_tile, sizeof l->uv_tile, fnv(l->heights.items, sizeof(f32) * l->heights.count)));
        const auto it = g_landscape_hash.find(mesh->mesh);
        if (it != g_landscape_hash.end() && it->second == hash && meshes.get(mesh->mesh)) continue;
        render::MeshData data;
        build_landscape_mesh(*l, data);
        meshes.set_generated(mesh->mesh, data);
        render::free_mesh(default_allocator(), data);
        g_landscape_hash[mesh->mesh] = hash;
    }
}

void generate_pcg(EditorState& s, Entity volume) {
    world::World& w = *s.world;
    auto* v = w.get<world::PcgVolume>(volume);
    if (!v) return;
    Entity out = v->output[0] ? find_named(w, v->output) : Entity{};
    if (!out.valid()) {
        char base[64], name[64];
        std::snprintf(base, sizeof base, "%s_Output", w.name(volume));
        w.unique_name(base, name, sizeof name);
        out = w.create(name);
        v = w.get<world::PcgVolume>(volume);
        std::snprintf(v->output, sizeof v->output, "%s", name);
    }
    const world::PcgVolume rules = *v;
    auto* ism = w.add<world::InstancedStaticMesh>(out);
    std::snprintf(ism->mesh, sizeof ism->mesh, "%s", rules.mesh);
    std::snprintf(ism->material, sizeof ism->material, "%s", rules.material);
    ism->color = rules.color;
    ism->instances.count = 0;
    const Mat4 m = w.world_matrix(volume), to_out = affine_inverse(w.world_matrix(out));
    const f32 area = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}) * length(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)});
    const u32 count = static_cast<u32>(std::fmin(static_cast<f32>(ism->instances.capacity()), rules.density * area / 100.0f));
    std::mt19937 rng(rules.seed);
    std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
    auto hit_below = [&](Vec3 top, Vec3 down, f32 reach, Vec3& at) {
        f32 t;
        if (!surface_ray(s, top, down, t) || t > reach) return false;
        at = top + down * t;
        return true;
    };
    for (u32 i = 0; i < count; ++i) {
        const f32 u = unit(rng) - 0.5f, vv = unit(rng) - 0.5f, yaw = unit(rng) * 360.0f, k = unit(rng);  // drawn every time: same seed, same layout
        const Vec3 top = transform_point(m, {u, vv, 1.0f}), bottom = transform_point(m, {u, vv, 0.0f});
        const f32 reach = length(bottom - top);
        if (reach < 1e-4f) continue;
        const Vec3 down = (bottom - top) * (1.0f / reach);
        Vec3 p, px, py;
        if (!hit_below(top, down, reach, p)) continue;
        Vec3 n{0, 0, 1};  // the surface's slope from two nearby hits
        if (hit_below(top + Vec3{0.2f, 0, 0}, down, reach, px) && hit_below(top + Vec3{0, 0.2f, 0}, down, reach, py)) {
            n = cross(px - p, py - p);
            if (length(n) > 1e-6f) n = normalize(n);
            if (n.z < 0.0f) n = n * -1.0f;
        }
        if (std::acos(n.z > 1.0f ? 1.0f : n.z) * 180.0f / kPi > rules.max_slope) continue;
        world::MeshInstance in;
        in.position = transform_point(to_out, p);
        const f32 turn = rules.random_yaw ? yaw : 0.0f;
        in.rotation = {0, 0, turn};
        if (rules.align_to_surface) {  // X then Y tilt the up axis onto the normal (seen before the turn)
            const f32 c = std::cos(-turn * kPi / 180.0f), sn = std::sin(-turn * kPi / 180.0f);
            const Vec3 local{n.x * c - n.y * sn, n.x * sn + n.y * c, n.z};
            in.rotation.x = -std::asin(local.y < -1.0f ? -1.0f : local.y > 1.0f ? 1.0f : local.y) * 180.0f / kPi;
            in.rotation.y = std::atan2(local.x, local.z) * 180.0f / kPi;
        }
        const f32 scale = rules.scale_min + (rules.scale_max - rules.scale_min) * k;
        in.scale = {scale, scale, scale};
        ism->instances.push_back(in);
    }
    g_pcg_hash[w.name(volume)] = pcg_hash(w, volume);
}

void update_pcg(EditorState& s) {
    if (s.playing || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;  // after the drag, not during it
    const reflect::TypeInfo& type = reflect::type_of<world::PcgVolume>();
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        const auto it = g_pcg_hash.find(s.world->name(e));
        if (it == g_pcg_hash.end() || it->second != pcg_hash(*s.world, e)) generate_pcg(s, e);
    }
}

} // namespace lsd::editor
