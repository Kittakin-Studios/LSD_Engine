#include "editor_modes.h"
#include "paper2d_core.h"
#include "landscape_core.h"
#include "mesh_edit.h"
#include "landscape_paint.h"
#include "tips.h"

#include "material_cache.h"
#include "mesh_cache.h"

#include "lsd/asset/texture.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/guid.h"
#include "lsd/foundation/allocator.h"
#include "lsd/particles/particles.h"
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"
#include "lsd/world/timeline.h"
#include "lsd/world/world.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
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
i32 g_edit_face = -1;   // the Editable Mesh face picked in the viewport
Entity g_edit_owner;
f32 g_extrude = 0.5f, g_inset = 0.25f, g_push = 0.1f;

world::EditableMesh* editable(EditorState& s) {
    return s.world->alive(s.selected) ? s.world->get<world::EditableMesh>(s.selected) : nullptr;
}

// The selected Editable Mesh's face tools.
void draw_mesh_edit(EditorState& s, world::EditableMesh& m) {
    if (g_edit_owner != s.selected) {
        g_edit_owner = s.selected;
        g_edit_face = -1;
    }
    ImGui::SeparatorText("Edit Mesh");
    ImGui::Text("%u faces, %u points", m.faces.count, m.points.count);
    if (g_edit_face < 0 || static_cast<u32>(g_edit_face) >= m.faces.count) {
        hint("Click a face of the mesh in the viewport to edit it.");
        return;
    }
    ImGui::Text("Face %d selected", g_edit_face + 1);
    const u32 face = static_cast<u32>(g_edit_face);
    auto apply = [&](bool ok, const char* what) {
        if (ok) mark_edited(s, what);
        else std::snprintf(s.status, sizeof s.status, "%s: the mesh is full (1024 points / faces) or the face is not usable", what);
    };
    ImGui::SetNextItemWidth(110);
    with_tip(ImGui::DragFloat("##extrude", &g_extrude, 0.01f, -100.0f, 100.0f, "%.2f m"), "How far Extrude and Bevel move the face out (negative: in)");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Extrude"), "Pulls the face out along its normal, with walls joining it to the rest (Unreal's PolyEdit Extrude)")) apply(extrude(m, face, g_extrude), "Extrude");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Bevel"), "Insets the face, then extrudes the smaller one: a chamfered step")) apply(bevel(m, face, g_inset, g_extrude), "Bevel");
    ImGui::SetNextItemWidth(110);
    with_tip(ImGui::DragFloat("##inset", &g_inset, 0.01f, 0.01f, 0.95f, "%.2f"), "How far Inset and Bevel shrink the face toward its centre (0..1)");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Inset"), "A smaller copy of the face inside it, ringed by new faces (windows, panels)")) apply(inset(m, face, g_inset), "Inset");
    ImGui::SetNextItemWidth(110);
    with_tip(ImGui::DragFloat("##push", &g_push, 0.01f, -100.0f, 100.0f, "%.2f m"), "How far Push / Pull moves the face");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Push / Pull"), "Moves the face along its normal; the faces around it stretch")) apply(push_pull(m, face, g_push), "Push / Pull");
    if (with_tip(ImGui::Button("Flip"), "Turns the face inside out (when it faces the wrong way)")) apply(flip_face(m, face), "Flip Face");
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Delete Face"), "Removes the face (an opening)")) {
        apply(delete_face(m, face), "Delete Face");
        g_edit_face = -1;
    }
    ImGui::SeparatorText("Mesh");
    with_tip(ImGui::DragFloat("UV Scale", &m.uv_scale, 0.01f, 0.01f, 100.0f, "%.2f"), "Texture tiles per metre");
    if (with_tip(ImGui::Button("Save as Mesh Asset"), "Writes it as an OBJ in Content/Meshes (a mesh asset you can place many times or open in Blender)")) {
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(s.registry ? s.registry->content_dir() : "Content") / "Meshes";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string base = s.world->name(s.selected);
        for (char& c : base) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
        }
        const fs::path file = dir / ("SM_" + base + ".obj");
        const std::string text = to_obj(m, base.c_str());
        FILE* f = std::fopen(file.string().c_str(), "wb");
        const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (f) std::fclose(f);
        if (ok && s.registry) s.registry->scan();
        std::snprintf(s.status, sizeof s.status, ok ? "Saved %s" : "Could not write %s", file.generic_string().c_str());
    }
}

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
    if (world::EditableMesh* m = editable(s)) {
        ImGui::Text("%s", s.world->name(s.selected));
        draw_mesh_edit(s, *m);
        if (with_tip(ImGui::Checkbox("Collision", &mesh->collision), "The player and physics bump into it")) mark_edited(s, "Collision");
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
    if (std::strcmp(mesh->mesh, "Cube") == 0 && with_tip(ImGui::Button("Convert to Editable Mesh"), "Makes the box editable face by face: extrude, inset, bevel (Unreal's PolyEdit)")) {
        world::EditableMesh* m = s.world->add<world::EditableMesh>(s.selected);
        make_box(*m);
        mesh = s.world->get<world::StaticMesh>(s.selected);
        mesh->mesh[0] = '\0';  // update_editable_meshes gives it its key
        mesh->collision_shape = 1;  // its own triangles
        g_edit_face = -1;
        mark_edited(s, "Convert to Editable Mesh");
    }
}

// ---- Foliage ----

f32 g_foliage_radius = 3.0f;
int g_foliage_selected = 0;  // the type the panel edits
std::mt19937 g_rng{12345};

f32 random01() { return std::uniform_real_distribution<f32>(0.0f, 1.0f)(g_rng); }

List<FoliageType, 8>& foliage_types(EditorState& s) {
    auto& types = s.settings.foliage_types;
    if (types.count == 0) types.push_back(FoliageType{});
    return types;
}

bool is_foliage(const world::World& w, Entity e, const char* mesh) {
    const auto* ism = w.get<world::InstancedStaticMesh>(e);
    return ism && std::strncmp(w.name(e), "Foliage", 7) == 0 && std::strcmp(ism->mesh, mesh) == 0;
}

// The surface's normal at `p` (found by two more rays beside it), for Align to Normal.
Vec3 surface_normal(EditorState& s, Vec3 p, f32 step) {
    f32 tx, ty;
    const Vec3 above{0, 0, step * 4.0f};
    if (!surface_ray(s, p + Vec3{step, 0, 0} + above, {0, 0, -1}, tx) || !surface_ray(s, p + Vec3{0, step, 0} + above, {0, 0, -1}, ty)) return {0, 0, 1};
    const Vec3 px = p + Vec3{step, 0, 0} + above + Vec3{0, 0, -tx}, py = p + Vec3{0, step, 0} + above + Vec3{0, 0, -ty};
    const Vec3 n = normalize(cross(px - p, py - p));
    return n.z > 0.05f ? n : Vec3{0, 0, 1};
}

// Euler degrees (X, then Y, then Z, as the Transform) that turn +Z onto `n` after a turn of `yaw` degrees.
Vec3 align_rotation(Vec3 n, f32 yaw) {
    const f32 c = radians(yaw);
    const Vec3 local{n.x * std::cos(-c) - n.y * std::sin(-c), n.x * std::sin(-c) + n.y * std::cos(-c), n.z};  // n before the yaw
    const f32 ax = std::asin(std::clamp(-local.y, -1.0f, 1.0f)), by = std::atan2(local.x, local.z);
    return {ax * 180.0f / kPi, by * 180.0f / kPi, yaw};
}

void foliage_stamp(EditorState& s, Vec3 center, bool erase) {
    world::World& w = *s.world;
    auto& types = foliage_types(s);
    const f32 radius = g_foliage_radius;
    bool changed = false;
    for (const FoliageType& type : types) {
        if (!type.enabled) continue;
        std::vector<Entity> layers;  // shortcut: std in editor-only code
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const Entity e = w.entity_at_slot(slot);
            if (e.valid() && is_foliage(w, e, type.mesh)) layers.push_back(e);
        }
        if (erase) {
            for (const Entity e : layers) {
                auto* ism = w.get<world::InstancedStaticMesh>(e);
                const Mat4 m = w.world_matrix(e);
                for (u32 k = ism->instances.count; k-- > 0;) {
                    const Vec3 p = transform_point(m, ism->instances[k].position);
                    if ((p.x - center.x) * (p.x - center.x) + (p.y - center.y) * (p.y - center.y) < radius * radius) {
                        ism->instances.remove(k);
                        changed = true;
                    }
                }
            }
            continue;
        }
        const f32 spacing = 1.0f / std::sqrt(std::fmax(type.density, 0.001f)) * 0.7f;
        const u32 tries = static_cast<u32>(std::fmin(24.0f, std::fmax(1.0f, type.density * kPi * radius * radius * 0.08f)));
        for (u32 i = 0; i < tries; ++i) {
            const f32 r = radius * std::sqrt(random01()), a = random01() * 2.0f * kPi;
            const Vec3 top{center.x + r * std::cos(a), center.y + r * std::sin(a), center.z + radius + 20.0f};
            f32 t;
            if (!surface_ray(s, top, {0, 0, -1}, t)) continue;
            const Vec3 p = top + Vec3{0, 0, -t};
            bool crowded = false;  // only its own kind: grass grows between the trees
            for (const Entity e : layers) {
                const auto* ism = w.get<world::InstancedStaticMesh>(e);
                const Mat4 m = w.world_matrix(e);
                for (u32 k = 0; k < ism->instances.count && !crowded; ++k) crowded = length(transform_point(m, ism->instances[k].position) - p) < spacing;
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
                std::snprintf(ism->mesh, sizeof ism->mesh, "%s", type.mesh);
                ism->color = type.color;
                ism->collision = type.collision;
                layers.push_back(layer);
            }
            world::MeshInstance in;
            in.position = transform_point(affine_inverse(w.world_matrix(layer)), p);
            const f32 yaw = type.random_yaw ? random01() * 360.0f : 0.0f;
            in.rotation = type.align_to_normal ? align_rotation(surface_normal(s, p, 0.25f), yaw) : Vec3{0, 0, yaw};
            const f32 k = type.scale_min + (type.scale_max - type.scale_min) * random01();
            in.scale = {k, k, k};
            w.get<world::InstancedStaticMesh>(layer)->instances.push_back(in);
            changed = true;
        }
    }
    if (changed) mark_edited(s, erase ? "Erase Foliage" : "Paint Foliage");
}

void draw_foliage(EditorState& s) {
    auto& types = foliage_types(s);
    ImGui::SeparatorText("Foliage Types");
    if (g_foliage_selected >= static_cast<int>(types.count)) g_foliage_selected = 0;
    for (u32 k = 0; k < types.count; ++k) {
        ImGui::PushID(static_cast<int>(k));
        with_tip(ImGui::Checkbox("##on", &types[k].enabled), "Painted (and erased) with the brush when ticked");
        ImGui::SameLine();
        char label[64];
        std::snprintf(label, sizeof label, "%s", types[k].mesh);
        if (ImGui::Selectable(label, g_foliage_selected == static_cast<int>(k))) g_foliage_selected = static_cast<int>(k);
        ImGui::SetItemTooltip("Click to edit this type below");
        ImGui::PopID();
    }
    if (with_tip(ImGui::Button("+ Type"), "Adds a foliage type (Unreal's + Foliage Type); up to 8") && types.count < types.capacity()) {
        types.push_back(FoliageType{});
        g_foliage_selected = static_cast<int>(types.count - 1);
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Remove"), "Removes the selected type from the list (its painted copies stay)") && types.count > 1) {
        types.remove(static_cast<u32>(g_foliage_selected));
        g_foliage_selected = 0;
    }
    FoliageType& t = types[static_cast<u32>(g_foliage_selected)];
    ImGui::SeparatorText("Selected Type");
    mesh_combo(s, "Mesh", t.mesh, sizeof t.mesh);
    with_tip(ImGui::ColorEdit3("Tint", &t.color.r), "Colour of the painted copies");
    with_tip(ImGui::DragFloat("Density (per m2)", &t.density, 0.02f, 0.01f, 20.0f, "%.2f"), "How many copies a stroke puts in each square metre");
    ImGui::DragFloatRange2("Scale", &t.scale_min, &t.scale_max, 0.01f, 0.05f, 10.0f, "%.2f");
    with_tip(ImGui::Checkbox("Random turn", &t.random_yaw), "Each copy gets a random turn so they do not all face the same way");
    with_tip(ImGui::Checkbox("Align to Normal", &t.align_to_normal), "Tilts each copy to the slope it lands on (grass, rocks); off keeps them upright (trees)");
    if (with_tip(ImGui::Checkbox("Collision", &t.collision), "Its copies block in Play (trees, rocks); off for grass. Also on the Foliage object's Instanced Static Mesh")) {
        for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {  // the copies already painted follow
            const Entity e = s.world->entity_at_slot(slot);
            if (e.valid() && is_foliage(*s.world, e, t.mesh)) s.world->get<world::InstancedStaticMesh>(e)->collision = t.collision;
        }
        mark_edited(s, "Foliage Collision");
    }
    ImGui::SeparatorText("Brush");
    with_tip(ImGui::DragFloat("Brush Size (m)", &g_foliage_radius, 0.1f, 0.2f, 100.0f, "%.1f"), "Radius of the brush circle in the viewport");
    hint("LMB drag paints every ticked type, each keeping its own spacing; Shift+LMB erases the ticked types.");
    hint("Copies go to Foliage objects (Instanced Static Meshes), one per mesh.");
    u32 total = 0;
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (e.valid() && is_foliage(*s.world, e, t.mesh)) total += s.world->get<world::InstancedStaticMesh>(e)->instances.count;
    }
    ImGui::Text("%u copies of this type's mesh", total);
}

// ---- Landscape ----

struct SculptBrush {
    int tool = 0;  // 0 Sculpt, 1 Smooth, 2 Flatten, 3 Paint
    f32 radius = 5.0f;
    f32 strength = 1.0f;
    int layer = 1;  // Paint: the layer the brush adds
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
            // Painted layers: one blended texture over the whole landscape; else the material tiles.
            const f32 u = l.layers.count ? (x + half) / l.size : x / tile, w = l.layers.count ? (half - y) / l.size : -y / tile;
            v = {{x, y, h(i, j)}, {n.x, n.y, n.z}, {u, w}};
        }
    }
    u32 k = 0;
    for (u32 j = 0; j + 1 < kGrid; ++j) {
        for (u32 i = 0; i + 1 < kGrid; ++i) {
            if (!quad_open(l, i, j)) continue;  // a hole: neither drawn nor collided
            const u32 a = j * kGrid + i, b = a + 1, c = a + kGrid, d = c + 1;
            for (const u32 index : {a, b, d, a, d, c}) data.indices[k++] = index;  // counter-clockwise from above
        }
    }
    data.index_count = k;
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

// Holes (Unreal's Landscape Visibility): cuts the terrain under the brush (Shift: fills it again).
void cut_holes(EditorState& s, Vec3 center) {
    const bool fill = ImGui::GetIO().KeyShift;
    bool changed = false;
    for (const Entity e : landscapes(*s.world)) {
        auto* l = s.world->get<world::Landscape>(e);
        const Mat4 m = s.world->world_matrix(e), inv = affine_inverse(m);
        const Vec3 c = transform_point(inv, center);
        const f32 sx = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
        const f32 radius = g_sculpt.radius / (sx > 1e-4f ? sx : 1.0f);
        const f32 step = l->size / static_cast<f32>(kGrid - 1), half = l->size * 0.5f;
        for (u32 j = 0; j < kGrid; ++j) {
            for (u32 i = 0; i < kGrid; ++i) {
                const f32 dx = -half + static_cast<f32>(i) * step - c.x, dy = -half + static_cast<f32>(j) * step - c.y;
                if (dx * dx + dy * dy > radius * radius || is_hole(*l, i, j) == !fill) continue;
                set_hole(*l, i, j, !fill);
                changed = true;
            }
        }
    }
    if (changed) mark_edited(s, fill ? "Fill Landscape Holes" : "Cut Landscape Holes");
}

// Paint (Unreal's Landscape Paint): adds the chosen layer under the brush (Shift: removes it), the
// others giving way so the weights still sum to 255.
void paint_layers(EditorState& s, Vec3 center, f32 dt) {
    const bool erase = ImGui::GetIO().KeyShift;
    bool changed = false;
    for (const Entity e : landscapes(*s.world)) {
        auto* l = s.world->get<world::Landscape>(e);
        if (l->layers.count < 2 || g_sculpt.layer >= static_cast<int>(l->layers.count)) continue;
        while (l->weights.count < kGrid * kGrid) l->weights.push_back(255u);  // all the first layer
        const Mat4 m = s.world->world_matrix(e), inv = affine_inverse(m);
        const Vec3 c = transform_point(inv, center);
        const f32 sx = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
        const f32 radius = g_sculpt.radius / (sx > 1e-4f ? sx : 1.0f);
        const f32 step = l->size / static_cast<f32>(kGrid - 1), half = l->size * 0.5f;
        const i32 i0 = static_cast<i32>(std::floor((c.x - radius + half) / step)), i1 = static_cast<i32>(std::ceil((c.x + radius + half) / step));
        const i32 j0 = static_cast<i32>(std::floor((c.y - radius + half) / step)), j1 = static_cast<i32>(std::ceil((c.y + radius + half) / step));
        for (i32 j = j0 < 0 ? 0 : j0; j <= j1 && j < static_cast<i32>(kGrid); ++j) {
            for (i32 i = i0 < 0 ? 0 : i0; i <= i1 && i < static_cast<i32>(kGrid); ++i) {
                const f32 dx = -half + static_cast<f32>(i) * step - c.x, dy = -half + static_cast<f32>(j) * step - c.y;
                const f32 d = std::sqrt(dx * dx + dy * dy);
                if (d > radius) continue;
                const f32 amount = std::fmin(1.0f, g_sculpt.strength * dt * 3.0f * 0.5f * (1.0f + std::cos(kPi * d / radius)));
                u32& packed = l->weights[static_cast<u32>(j) * kGrid + static_cast<u32>(i)];
                const u32 out = paint_weight(packed, g_sculpt.layer, amount, erase);
                if (out != packed) {
                    packed = out;
                    changed = true;
                }
            }
        }
    }
    if (changed) mark_edited(s, "Paint Landscape");
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
    ImGui::SameLine();
    with_tip(ImGui::RadioButton("Paint", &g_sculpt.tool, 3), "Paints a layer (Unreal's Landscape Paint): LMB adds it, Shift+LMB removes it");
    ImGui::SameLine();
    with_tip(ImGui::RadioButton("Holes", &g_sculpt.tool, 4), "Cuts holes for caves and tunnels (Unreal's Visibility tool): LMB cuts, Shift+LMB fills; no drawing or collision there");
    with_tip(ImGui::DragFloat("Brush Size (m)", &g_sculpt.radius, 0.1f, 0.5f, 200.0f, "%.1f"), "Radius of the brush circle in the viewport");
    with_tip(ImGui::DragFloat("Strength", &g_sculpt.strength, 0.01f, 0.05f, 5.0f, "%.2f"), "How much each stroke raises, lowers or smooths");
    if (g_sculpt.tool == 3) {
        const auto* first = s.world->get<world::Landscape>(s.world->alive(s.selected) && s.world->get<world::Landscape>(s.selected) ? s.selected : all[0]);
        ImGui::SeparatorText("Layers");
        if (!first || first->layers.count < 2) {
            hint("Select the landscape and add 2 or more Layers in Details > Landscape > Layers (a texture, tint and tile size each).");
        } else {
            for (u32 k = 0; k < first->layers.count; ++k) {
                char label[64];
                std::snprintf(label, sizeof label, "%s##layer%u", first->layers[k].name[0] ? first->layers[k].name : "Layer", k);
                ImGui::RadioButton(label, &g_sculpt.layer, static_cast<int>(k));
                ImGui::SetItemTooltip("Paint this layer (layer %u)%s", k + 1, k == 0 ? "; the first covers everything at the start" : "");
            }
        }
        hint("LMB drag paints the layer, Shift+LMB removes it.");
    } else {
        hint("LMB drag on the terrain sculpts (Shift lowers).");
    }
    ImGui::SeparatorText("Heightmap");
    static char heightmap[260] = "Content/Heightmap.png";
    static f32 max_height = 50.0f;
    ImGui::SetNextItemWidth(220);
    with_tip(ImGui::InputText("File", heightmap, sizeof heightmap), "A greyscale image (PNG, JPG, TGA...): black is 0, white is Max Height; it is stretched over the landscape");
    with_tip(ImGui::DragFloat("Max Height (m)", &max_height, 0.5f, 0.1f, 2000.0f, "%.1f"), "The height white stands for");
    Entity target = s.world->alive(s.selected) && s.world->get<world::Landscape>(s.selected) ? s.selected : all[0];
    if (with_tip(ImGui::Button("Import Heightmap"), "Sets the selected landscape's heights from the image (undoable; 8 bits: 256 steps)")) {
        asset::TextureData image(default_allocator());
        if (asset::load_texture(heightmap, false, 4096, image)) {
            heights_from_image(*s.world->get<world::Landscape>(target), image.mip(0), image.width, image.height, max_height);
            mark_edited(s, "Import Heightmap");
            std::snprintf(s.status, sizeof s.status, "Imported %s (%u x %u) into %s", heightmap, image.width, image.height, s.world->name(target));
        } else {
            std::snprintf(s.status, sizeof s.status, "Could not read the image %s", heightmap);
        }
    }
    ImGui::SameLine();
    if (with_tip(ImGui::Button("Export Heightmap"), "Writes the selected landscape's heights as a greyscale PNG (65 x 65, 0..Max Height)")) {
        std::vector<u8> pixels(static_cast<usize>(kGrid) * kGrid * 4);
        image_from_heights(*s.world->get<world::Landscape>(target), max_height, pixels.data());
        const bool ok = asset::write_png(heightmap, kGrid, kGrid, pixels.data());
        std::snprintf(s.status, sizeof s.status, ok ? "Exported %s" : "Could not write %s", heightmap);
    }
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
    u64 h = fnv(m.m, sizeof m.m, fnv(&rules, sizeof rules));
    const reflect::TypeInfo& type = reflect::type_of<world::PcgVolume>();
    for (u32 i = 0; i < w.count(type); ++i) {  // moving an exclusion box rebuilds the volumes around it
        const Entity other = w.owner(type, i);
        if (other == e || !static_cast<const world::PcgVolume*>(w.at(type, i))->exclude) continue;
        const Mat4 x = w.world_matrix(other);
        h = fnv(x.m, sizeof x.m, h);
    }
    return h;
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
u32 g_tile_layer = 0;        // the layer it paints: 0 the map's own tiles, 1.. its Layers

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
    // Layers (Unreal's Tile Map layers): Main is the map's own tiles, the others its Layers list.
    if (g_tile_layer > map->layers.count) g_tile_layer = 0;
    const char* layer_name = g_tile_layer ? map->layers[g_tile_layer - 1].name : "Main";
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Layer", layer_name)) {
        if (ImGui::Selectable("Main", g_tile_layer == 0)) g_tile_layer = 0;
        for (u32 k = 0; k < map->layers.count; ++k) {
            char label[48];
            std::snprintf(label, sizeof label, "%s##layer%u", map->layers[k].name, k);
            if (ImGui::Selectable(label, g_tile_layer == k + 1)) g_tile_layer = k + 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The layer the brush paints: Main (it collides), or one of the map's Layers (depth, visibility and collision in Details)");
    ImGui::SameLine();
    if (with_tip(ImGui::SmallButton("+"), "Adds a layer (up to 3) in front of the main tiles") && map->layers.count < map->layers.capacity()) {
        world::TileLayer layer;
        std::snprintf(layer.name, sizeof layer.name, "Layer %u", map->layers.count + 1);
        layer.depth = -0.1f * static_cast<f32>(map->layers.count + 1);
        map->layers.push_back(layer);
        g_tile_layer = map->layers.count;
        mark_edited(s, "Add Tile Layer");
    }
    ImGui::SameLine();
    if (with_tip(ImGui::SmallButton("-"), "Removes the chosen layer and its tiles (undoable)") && g_tile_layer > 0) {
        for (u32 k = g_tile_layer - 1; k + 1 < map->layers.count; ++k) map->layers[k] = map->layers[k + 1];
        --map->layers.count;
        g_tile_layer = 0;
        mark_edited(s, "Remove Tile Layer");
    }
    if (g_tile_layer > 0) {
        world::TileLayer& layer = map->layers[g_tile_layer - 1];
        if (with_tip(ImGui::Checkbox("Visible", &layer.visible), "Shows or hides the layer")) mark_edited(s, "Tile Layer");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (with_tip(ImGui::DragFloat("Depth", &layer.depth, 0.01f, -10.0f, 10.0f, "%.2f m"), "Negative: in front of the main tiles; positive: behind")) mark_edited(s, "Tile Layer");
    }
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
    // The chosen tile's collision (Paper2D's per-tile collision), kept on the map for every painted copy.
    static const char* const kShapes[] = {"Full", "None", "Top Half", "Bottom Half", "Slope Up", "Slope Down", "One Way"};
    int shape = static_cast<int>(tile_shape(*map, static_cast<i32>(g_tile) + 1));
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Tile Collision", &shape, kShapes, 7)) {
        while (map->tile_shapes.count <= g_tile && map->tile_shapes.count < map->tile_shapes.capacity()) map->tile_shapes.push_back({});
        if (g_tile < map->tile_shapes.count) {
            map->tile_shapes[g_tile].shape = static_cast<u32>(shape);
            mark_edited(s, "Tile Collision");
        }
    }
    ImGui::SetItemTooltip("How the chosen tile blocks in Play: Full, None (decoration), Top / Bottom Half, a Slope rising to the right (Up) or left (Down), or One Way (jump up through it, land on top)");
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
        List<i32, 4096>* tiles = layer_tiles(*map, g_tile_layer);
        if (!tiles) tiles = &map->tiles;
        const u32 cells = map->width * map->height < tiles->capacity() ? map->width * map->height : tiles->capacity();
        while (tiles->count < cells) tiles->push_back(0);
        const u32 index = static_cast<u32>(tz) * map->width + static_cast<u32>(tx);
        const i32 value = io.KeyShift ? 0 : static_cast<i32>(g_tile) + 1;
        if (index < tiles->count && (*tiles)[index] != value) {
            (*tiles)[index] = value;
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

// Modeling: the selected Editable Mesh's faces are picked with a click, the picked one outlined.
bool face_brush(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, bool hovered) {
    world::EditableMesh* m = editable(s);
    if (!m) return false;
    const Mat4 world_m = s.world->world_matrix(s.selected), inv = affine_inverse(world_m);
    const ImGuiIO& io = ImGui::GetIO();
    if (g_edit_owner == s.selected && g_edit_face >= 0 && static_cast<u32>(g_edit_face) < m->faces.count) {
        const world::EditFace& f = m->faces[static_cast<u32>(g_edit_face)];
        ImVec2 outline[4];
        u32 n = 0;
        for (u32 i = 0; i < face_size(f); ++i) {
            if (project(view_proj, transform_point(world_m, face_point(*m, f, i)), pos, size, outline[n])) ++n;
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        if (n == face_size(f)) {
            dl->AddConvexPolyFilled(outline, static_cast<int>(n), IM_COL32(243, 195, 18, 70));
            dl->AddPolyline(outline, static_cast<int>(n), IM_COL32(243, 195, 18, 255), ImDrawFlags_Closed, 2.0f);
        }
        dl->PopClipRect();
    }
    if (!hovered || io.KeyAlt || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
    Vec3 o, d;
    viewport_ray(s.camera, pos, size, io.MousePos, o, d);
    f32 t = 0.0f;
    const i32 face = pick_face(*m, transform_point(inv, o), transform_vector(inv, d), t);
    if (face < 0) return false;  // missed the mesh: a normal click (select something else)
    g_edit_owner = s.selected;
    g_edit_face = face;
    return true;
}

bool mode_brush(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, bool hovered) {
    if (s.mode == EditorMode::Paper2D) return tile_brush(s, view_proj, pos, size, hovered);
    if (s.mode == EditorMode::Modeling) return face_brush(s, view_proj, pos, size, hovered);
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
    const f32 radius = s.mode == EditorMode::Foliage ? g_foliage_radius : g_sculpt.radius;
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
            if (g_sculpt.tool == 3) paint_layers(s, p, io.DeltaTime < 0.1f ? io.DeltaTime : 0.1f);
            else if (g_sculpt.tool == 4) cut_holes(s, p);
            else sculpt(s, p, io.DeltaTime < 0.1f ? io.DeltaTime : 0.1f, flatten_z);
        }
    }
    return true;
}

// ---- Landscape layers: blended on the CPU into one colour texture per landscape ----
// shortcut: one 1024 texture over the whole terrain (detail is limited by it: about 16 texels a metre
// on a 64 m landscape); blend in the shader with a weight map when terrains get big.

struct LandscapeTexture {
    u64 hash = 0;
    rhi::TextureHandle texture;
    u64 built_ms = 0;
};
std::unordered_map<std::string, LandscapeTexture> g_landscape_textures;  // mesh key -> its blended layers
struct LayerImage {
    std::vector<u8> pixels;  // RGBA8 sRGB, mip 0
    u32 width = 0, height = 0;
    i64 modified = 0;
};
std::unordered_map<std::string, LayerImage> g_layer_images;  // texture GUID -> its pixels

const LayerImage* layer_image(const asset::AssetRegistry& registry, const char* guid_text) {
    asset::Guid guid;
    if (!guid_text[0] || !asset::parse_guid(guid_text, guid)) return nullptr;
    const asset::AssetRecord* r = registry.find(guid);
    if (!r) return nullptr;
    char path[300];
    registry.source_path(*r, path, sizeof path);
    std::error_code ec;
    const i64 modified = static_cast<i64>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
    LayerImage& image = g_layer_images[guid_text];
    if (image.width && image.modified == modified) return &image;
    asset::TextureData data(default_allocator());
    if (!asset::load_texture(path, true, 1024, data)) return nullptr;
    image.width = data.width;
    image.height = data.height;
    image.pixels.assign(data.mip(0), data.mip(0) + static_cast<usize>(data.width) * data.height * 4);
    image.modified = modified;
    return &image;
}

u8 to_srgb_byte(f32 linear) {
    const f32 c = std::clamp(linear, 0.0f, 1.0f);
    return static_cast<u8>(std::lround((c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) * 255.0f));
}

void blend_landscape_layers(const world::Landscape& l, const asset::AssetRegistry& registry, u32 size, std::vector<u8>& out) {
    out.assign(static_cast<usize>(size) * size * 4, 255);
    const LayerImage* images[4] = {};
    u8 tint[4][3];
    for (u32 k = 0; k < l.layers.count; ++k) {
        images[k] = layer_image(registry, l.layers[k].texture);
        tint[k][0] = to_srgb_byte(l.layers[k].tint.r);
        tint[k][1] = to_srgb_byte(l.layers[k].tint.g);
        tint[k][2] = to_srgb_byte(l.layers[k].tint.b);
    }
    auto weights_at = [&](u32 i, u32 j, f32 w[4]) {
        const u32 packed = l.weights.count >= kGrid * kGrid ? l.weights[j * kGrid + i] : 255u;
        for (int k = 0; k < 4; ++k) w[k] = static_cast<f32>((packed >> (8 * k)) & 0xFFu) / 255.0f;
    };
    for (u32 y = 0; y < size; ++y) {
        // Texel (x, y): row 0 is the landscape's +Y edge (the mesh's UV v runs along -Y).
        const f32 gy = (1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size)) * static_cast<f32>(kGrid - 1);
        const u32 j = std::min(static_cast<u32>(gy), kGrid - 2);
        const f32 fy = gy - static_cast<f32>(j);
        for (u32 x = 0; x < size; ++x) {
            const f32 gx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size) * static_cast<f32>(kGrid - 1);
            const u32 i = std::min(static_cast<u32>(gx), kGrid - 2);
            const f32 fx = gx - static_cast<f32>(i);
            f32 w00[4], w10[4], w01[4], w11[4], w[4];
            weights_at(i, j, w00);
            weights_at(i + 1, j, w10);
            weights_at(i, j + 1, w01);
            weights_at(i + 1, j + 1, w11);
            for (int k = 0; k < 4; ++k) w[k] = (w00[k] * (1 - fx) + w10[k] * fx) * (1 - fy) + (w01[k] * (1 - fx) + w11[k] * fx) * fy;
            const f32 wx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size) * l.size, wy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size) * l.size;  // metres from the corner
            f32 rgb[3] = {};
            for (u32 k = 0; k < l.layers.count; ++k) {
                if (w[k] <= 0.0f) continue;
                u8 texel[3] = {255, 255, 255};
                if (const LayerImage* im = images[k]) {
                    const f32 tile = l.layers[k].tile > 0.1f ? l.layers[k].tile : 0.1f;
                    const f32 u = wx / tile, v = wy / tile;
                    const u32 tx = static_cast<u32>((u - std::floor(u)) * static_cast<f32>(im->width)) % im->width;
                    const u32 ty = static_cast<u32>((v - std::floor(v)) * static_cast<f32>(im->height)) % im->height;
                    const u8* p = &im->pixels[(static_cast<usize>(ty) * im->width + tx) * 4];
                    texel[0] = p[0];
                    texel[1] = p[1];
                    texel[2] = p[2];
                }
                for (int c = 0; c < 3; ++c) rgb[c] += w[k] * static_cast<f32>(texel[c]) * static_cast<f32>(tint[k][c]) / 255.0f;
            }
            u8* o = &out[(static_cast<usize>(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>(std::lround(std::clamp(rgb[c], 0.0f, 255.0f)));
        }
    }
}

rhi::TextureHandle landscape_texture(const char* mesh_key) {
    const auto it = g_landscape_textures.find(mesh_key);
    return it == g_landscape_textures.end() ? rhi::TextureHandle{} : it->second.texture;
}

void update_landscape_texture(const world::Landscape& l, const char* key, MeshCache& meshes) {
    LandscapeTexture& t = g_landscape_textures[key];
    if (l.layers.count == 0) {
        if (t.texture.valid()) meshes.device().destroy_texture(t.texture);
        t = LandscapeTexture{};
        return;
    }
    u64 hash = fnv(&l.size, sizeof l.size, fnv(l.layers.items, sizeof(world::LandscapeLayer) * l.layers.count, fnv(l.weights.items, sizeof(u32) * l.weights.count)));
    for (u32 k = 0; k < l.layers.count; ++k) {  // a layer's texture file edited in Photoshop: blend again
        if (const auto im = g_layer_images.find(l.layers[k].texture); im != g_layer_images.end()) hash = fnv(&im->second.modified, sizeof im->second.modified, hash);
    }
    const u64 now = SDL_GetTicks();
    if (t.texture.valid() && t.hash == hash) return;
    if (t.texture.valid() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && now - t.built_ms < 250) return;  // painting: a few times a second
    static std::vector<u8> pixels;
    constexpr u32 kSize = 1024;
    blend_landscape_layers(l, meshes.registry(), kSize, pixels);
    asset::TextureData data(default_allocator());
    if (!asset::build_texture(pixels.data(), kSize, kSize, true, kSize, data)) return;
    const void* mips[asset::kMaxTextureMips] = {};
    for (u32 m = 0; m < data.mip_count; ++m) mips[m] = data.mip(m);
    const rhi::TextureHandle fresh = meshes.device().create_texture(data.width, data.height, data.mip_count, rhi::TextureFormat::RGBA8_sRGB, mips);
    if (t.texture.valid()) meshes.device().destroy_texture(t.texture);  // shortcut: freed at once, like the reflection atlas
    t.texture = fresh;
    t.hash = hash;
    t.built_ms = now;
}

std::unordered_map<std::string, u64> g_edit_mesh_hash;  // mesh key -> points and faces it was built from

void update_editable_meshes(world::World& w, MeshCache& meshes) {
    const reflect::TypeInfo& type = reflect::type_of<world::EditableMesh>();
    if (!w.count(type)) return;
    std::vector<Entity> owners;
    for (u32 i = 0; i < w.count(type); ++i) owners.push_back(w.owner(type, i));
    std::unordered_set<std::string> keys;  // a duplicated mesh gets a key of its own
    for (const Entity e : owners) {
        world::StaticMesh* mesh = w.get<world::StaticMesh>(e);
        if (!mesh) {
            mesh = w.add<world::StaticMesh>(e);
            mesh->collision_shape = 1;
            mesh->mesh[0] = '\0';
        }
        if (std::strncmp(mesh->mesh, "EditMesh:", 9) != 0 || keys.count(mesh->mesh)) {
            static std::mt19937 ids{std::random_device{}()};
            std::snprintf(mesh->mesh, sizeof mesh->mesh, "EditMesh:%08x", static_cast<unsigned>(ids()));
        }
        keys.insert(mesh->mesh);
        const auto* m = w.get<world::EditableMesh>(e);
        const u64 hash = fnv(&m->uv_scale, sizeof m->uv_scale, fnv(m->faces.items, sizeof(world::EditFace) * m->faces.count, fnv(m->points.items, sizeof(world::EditPoint) * m->points.count)));
        const auto it = g_edit_mesh_hash.find(mesh->mesh);
        if (it != g_edit_mesh_hash.end() && it->second == hash && meshes.get(mesh->mesh)) continue;
        render::MeshData data;
        build_edit_mesh(*m, data);
        meshes.set_generated(mesh->mesh, data);
        render::free_mesh(default_allocator(), data);
        g_edit_mesh_hash[mesh->mesh] = hash;
    }
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
        const u64 hash = fnv(l->holes.items, sizeof(u32) * l->holes.count,
                             fnv(&l->layers.count, sizeof l->layers.count, fnv(&l->size, sizeof l->size, fnv(&l->uv_tile, sizeof l->uv_tile, fnv(l->heights.items, sizeof(f32) * l->heights.count)))));
        const auto it = g_landscape_hash.find(mesh->mesh);
        if (it != g_landscape_hash.end() && it->second == hash && meshes.get(mesh->mesh)) continue;
        render::MeshData data;
        build_landscape_mesh(*l, data);
        meshes.set_generated(mesh->mesh, data);
        render::free_mesh(default_allocator(), data);
        g_landscape_hash[mesh->mesh] = hash;
    }
    for (const Entity e : landscapes(w)) {  // their painted layers (after the meshes: a new key exists now)
        if (const world::StaticMesh* mesh = w.get<world::StaticMesh>(e)) update_landscape_texture(*w.get<world::Landscape>(e), mesh->mesh, meshes);
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
    // One Instanced Static Mesh per mesh: the output for the main one, its "Variant N" children for More Meshes.
    std::vector<Entity> outputs{out};
    for (u32 k = 1; k <= world::PcgVolume{}.more_meshes.capacity(); ++k) {
        char name[32];
        std::snprintf(name, sizeof name, "Variant %u", k);
        Entity child;
        for (u32 slot = 0; slot < w.slot_count() && !child.valid(); ++slot) {
            const Entity c = w.entity_at_slot(slot);
            if (c.valid() && w.parent(c) == out && std::strcmp(w.name(c), name) == 0) child = c;
        }
        if (!child.valid() && k <= rules.more_meshes.count && !rules.exclude) {
            child = w.create(name);
            w.set_parent(child, out, false);
        }
        if (child.valid()) {
            if (k > rules.more_meshes.count || rules.exclude) w.add<world::InstancedStaticMesh>(child)->instances.count = 0;  // no longer used
            else outputs.push_back(child);
        }
    }
    for (u32 k = 0; k < outputs.size(); ++k) {
        auto* target = w.add<world::InstancedStaticMesh>(outputs[k]);
        std::snprintf(target->mesh, sizeof target->mesh, "%s", k ? rules.more_meshes[k - 1].mesh : rules.mesh);
        std::snprintf(target->material, sizeof target->material, "%s", k ? rules.more_meshes[k - 1].material : rules.material);
        target->color = k ? rules.more_meshes[k - 1].color : rules.color;
        target->instances.count = 0;
    }
    if (rules.exclude) {  // an exclusion box scatters nothing
        g_pcg_hash[w.name(volume)] = pcg_hash(w, volume);
        return;
    }
    std::vector<Mat4> excluded;  // the exclusion boxes, as world-to-box
    {
        const reflect::TypeInfo& type = reflect::type_of<world::PcgVolume>();
        for (u32 i = 0; i < w.count(type); ++i) {
            if (w.owner(type, i) != volume && static_cast<const world::PcgVolume*>(w.at(type, i))->exclude) excluded.push_back(affine_inverse(w.world_matrix(w.owner(type, i))));
        }
    }
    f32 total_weight = rules.weight > 0.0f ? rules.weight : 0.0f;
    for (const world::PcgMesh& extra : rules.more_meshes) total_weight += extra.weight > 0.0f ? extra.weight : 0.0f;
    auto* ism = w.get<world::InstancedStaticMesh>(out);
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
        u32 pick = 0;  // which mesh (drawn only with More Meshes, so a single-mesh layout stays as it was)
        if (rules.more_meshes.count && total_weight > 0.0f) {
            f32 r = unit(rng) * total_weight - (rules.weight > 0.0f ? rules.weight : 0.0f);
            while (r >= 0.0f && pick < rules.more_meshes.count) {
                ++pick;
                if (pick <= rules.more_meshes.count) r -= rules.more_meshes[pick - 1].weight > 0.0f ? rules.more_meshes[pick - 1].weight : 0.0f;
            }
            if (pick > rules.more_meshes.count) pick = rules.more_meshes.count;
        }
        bool inside = false;
        for (const Mat4& x : excluded) {
            const Vec3 q = transform_point(x, p);
            inside |= q.x >= -0.5f && q.x <= 0.5f && q.y >= -0.5f && q.y <= 0.5f && q.z >= 0.0f && q.z <= 1.0f;
        }
        if (inside) continue;
        if (pick >= outputs.size()) pick = 0;
        auto* target = pick ? w.get<world::InstancedStaticMesh>(outputs[pick]) : ism;
        if (pick) in.position = transform_point(affine_inverse(w.world_matrix(outputs[pick])), p);
        if (target->instances.count < target->instances.capacity()) target->instances.push_back(in);
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
