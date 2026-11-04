#include "prefab_editor.h"
#include "tips.h"
#include "workspaces.h"

#include "mesh_cache.h"

#include "lsd/debug_ui/details_panel.h"
#include "lsd/foundation/allocator.h"
#include "lsd/world/components.h"
#include "lsd/world/prefab.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

constexpr u32 kSize = 512;

struct PrefabWindow {
    bool open = false;
    bool dirty = false;
    char name[96] = "";
    char path[300] = "";
    world::World* world = nullptr;
    world::Entity root, selected;
    f32 yaw = -2.4f, pitch = 0.45f, zoom = 1.0f;
    rhi::TextureHandle target;
    rhi::Device* device = nullptr;
    EditorState* state = nullptr;
};
PrefabWindow g_pw;

void close_window() {
    delete g_pw.world;
    g_pw.world = nullptr;
    g_pw.open = false;
}

void tree(world::World& w, world::Entity e) {
    bool has_children = false;
    for (u32 slot = 0; slot < w.slot_count() && !has_children; ++slot) {
        const world::Entity c = w.entity_at_slot(slot);
        has_children = c.valid() && w.parent(c) == e;
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | (has_children ? 0 : ImGuiTreeNodeFlags_Leaf);
    if (e == g_pw.selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(e.value)), flags, "%s%s", w.name(e), e == g_pw.root ? "  (root)" : "");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_pw.selected = e;
    if (!open) return;
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const world::Entity c = w.entity_at_slot(slot);
        if (c.valid() && w.parent(c) == e) tree(w, c);
    }
    ImGui::TreePop();
}

// The prefab's meshes' bounds (world space), for framing the preview.
bool bounds(EditorState& s, Vec3& lo, Vec3& hi) {
    bool any = false;
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    for (u32 i = 0; i < g_pw.world->count(type); ++i) {
        const CachedMesh* mesh = s.meshes ? s.meshes->get(static_cast<const world::StaticMesh*>(g_pw.world->at(type, i))->mesh) : nullptr;
        if (!mesh) continue;
        const Mat4 m = g_pw.world->world_matrix(g_pw.world->owner(type, i));
        for (int c = 0; c < 8; ++c) {
            const Vec3 p = transform_point(m, Vec3{(c & 1) ? mesh->bounds_max.x : mesh->bounds_min.x, (c & 2) ? mesh->bounds_max.y : mesh->bounds_min.y,
                                                   (c & 4) ? mesh->bounds_max.z : mesh->bounds_min.z});
            lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
            hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
            any = true;
        }
    }
    return any;
}

} // namespace

void open_prefab_editor(EditorState& s, const asset::AssetRecord& record) {
    close_window();
    s.registry->source_path(record, g_pw.path, sizeof g_pw.path);
    g_pw.world = new world::World(default_allocator());
    g_pw.root = g_pw.world->create(record.name);
    if (!world::update_instance_from_file(*g_pw.world, g_pw.root, g_pw.path, true)) {
        close_window();
        std::snprintf(s.status, sizeof s.status, "Could not open %s", record.name);
        return;
    }
    g_pw.world->remove(g_pw.root, reflect::type_of<world::PrefabInstance>());  // edited as the prefab itself
    std::snprintf(g_pw.name, sizeof g_pw.name, "%s", record.name);
    g_pw.selected = g_pw.root;
    g_pw.open = true;
    g_pw.dirty = false;
    g_pw.zoom = 1.0f;
    ImGui::SetWindowFocus("###PrefabEditor");
}

void render_prefab_editor(rhi::Device& device, const f32 background[4]) {
    if (!g_pw.open || !g_pw.state || !g_pw.world) return;
    EditorState& s = *g_pw.state;
    g_pw.device = &device;
    if (!g_pw.target.valid()) g_pw.target = device.create_render_target(kSize, kSize);
    if (!g_pw.target.valid()) return;
    Vec3 lo, hi;
    if (!bounds(s, lo, hi)) lo = Vec3{-1, -1, 0}, hi = Vec3{1, 1, 2};
    const Vec3 centre = (lo + hi) * 0.5f;
    const f32 radius = std::fmax(length(hi - lo) * 0.5f, 0.05f);
    const f32 distance = radius / std::sin(0.3f) * 1.05f * g_pw.zoom;
    const Vec3 dir{std::cos(g_pw.pitch) * std::cos(g_pw.yaw), std::cos(g_pw.pitch) * std::sin(g_pw.yaw), std::sin(g_pw.pitch)};
    const Vec3 eye = centre + dir * distance;
    const Mat4 view_proj = perspective(0.6f, 1.0f, distance * 0.01f, distance + radius * 4.0f) * look_at(eye, centre, Vec3{0, 0, 1});
    const f32 dark[4] = {background[0] * 0.7f, background[1] * 0.7f, background[2] * 0.7f, 1.0f};
    device.set_render_target(g_pw.target, dark);
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    for (u32 i = 0; i < g_pw.world->count(type); ++i) {  // shortcut: static meshes with their colour; materials and skinned meshes show in the level
        const auto* sm = static_cast<const world::StaticMesh*>(g_pw.world->at(type, i));
        const CachedMesh* mesh = s.meshes ? s.meshes->get(sm->mesh) : nullptr;
        if (!mesh) continue;
        const world::Entity e = g_pw.world->owner(type, i);
        const Mat4 m = g_pw.world->world_matrix(e);
        const Mat4 mvp = view_proj * m;
        const Mat4 inv = affine_inverse(m);
        rhi::MeshDraw draw{};
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
        const bool selected = e == g_pw.selected;
        draw.color[0] = selected ? 1.0f : sm->color.r;
        draw.color[1] = selected ? 0.85f : sm->color.g;
        draw.color[2] = selected ? 0.25f : sm->color.b;
        draw.color[3] = 1.0f;
        device.draw_mesh(draw);
    }
    device.set_render_target({}, nullptr);
}

void draw_prefab_editor(EditorState& s) {
    g_pw.state = &s;
    if (!g_pw.open || !g_pw.world) return;
    world::World& w = *g_pw.world;
    char title[160];
    std::snprintf(title, sizeof title, "%s%s - Prefab###PrefabEditor", g_pw.name, g_pw.dirty ? "*" : "");
    ImGui::SetNextWindowSize(ImVec2(1100, 620), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!begin_editor(title, &open)) return;  // a panel tab, or the workspace's editor area
    if (!open) {
        ImGui::End();
        close_window();
        return;
    }
    if (with_tip(ImGui::Button("Save"), "Writes the prefab: every copy in every level updates")) {
        if (world::save_prefab_file(w, g_pw.root, g_pw.path)) {
            g_pw.dirty = false;
            const u32 updated = refresh_prefabs(s);  // the level's copies take it (keeping their overrides)
            std::snprintf(s.status, sizeof s.status, "%s saved; %u placed cop%s updated", g_pw.name, updated, updated == 1 ? "y" : "ies");
        } else {
            std::snprintf(s.status, sizeof s.status, "Could not save %s", g_pw.path);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drag the preview to turn it, wheel to zoom. Changes reach the level's copies when you Save.");
    // Preview
    ImGui::BeginChild("preview", ImVec2(ImGui::GetContentRegionAvail().x * 0.45f, 0), ImGuiChildFlags_Borders);
    const f32 side = std::fmin(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 4.0f);
    if (g_pw.target.valid() && s.device) {
        const bool flipped = s.device->render_targets_flipped();
        ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(g_pw.target)), ImVec2(side, side), ImVec2(0, flipped ? 1.0f : 0.0f), ImVec2(1, flipped ? 0.0f : 1.0f));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                g_pw.yaw -= ImGui::GetIO().MouseDelta.x * 0.01f;
                g_pw.pitch = std::fmin(std::fmax(g_pw.pitch + ImGui::GetIO().MouseDelta.y * 0.01f, -1.5f), 1.5f);
            }
            g_pw.zoom = std::fmin(std::fmax(g_pw.zoom * (1.0f - ImGui::GetIO().MouseWheel * 0.1f), 0.2f), 5.0f);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Objects
    ImGui::BeginChild("objects", ImVec2(220, 0), ImGuiChildFlags_Borders);
    tree(w, g_pw.root);
    ImGui::Separator();
    if (with_tip(ImGui::Button("+ Child"), "Adds an object under the selected one") && w.alive(g_pw.selected)) {
        char name[64];
        w.unique_name("Part", name, sizeof name);
        const world::Entity child = w.create(name);
        w.set_parent(child, g_pw.selected, false);
        g_pw.selected = child;
        g_pw.dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_pw.selected == g_pw.root);
    if (with_tip(ImGui::Button("Delete"), "Removes the selected object from the prefab") && w.alive(g_pw.selected)) {
        w.destroy(g_pw.selected);
        g_pw.selected = g_pw.root;
        g_pw.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();
    // Components of the selected object
    ImGui::BeginChild("components", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (w.alive(g_pw.selected)) {
        char name[64];
        std::snprintf(name, sizeof name, "%s", w.name(g_pw.selected));
        if (with_tip(ImGui::InputText("Name", name, sizeof name), "The selected object's name")) {
            w.set_name(g_pw.selected, name);
            g_pw.dirty = true;
        }
        u32 type_count = 0;
        const reflect::TypeInfo* const* types = world::component_types(type_count);
        for (u32 i = 0; i < type_count; ++i) {
            void* component = w.get(g_pw.selected, *types[i]);
            if (!component || types[i] == &reflect::type_of<world::PrefabInstance>()) continue;
            ImGui::PushID(types[i]->name);
            bool keep = true;
            const bool shown = ImGui::CollapsingHeader(types[i]->name, i == 0 ? nullptr : &keep, ImGuiTreeNodeFlags_DefaultOpen);
            if (shown && draw_details(*types[i], component)) g_pw.dirty = true;
            if (!keep) {
                w.remove(g_pw.selected, *types[i]);
                g_pw.dirty = true;
            }
            ImGui::PopID();
        }
        if (with_tip(ImGui::Button("+ Add Component", ImVec2(-1, 0)), "Adds a component to the selected object")) ImGui::OpenPopup("add");
        if (ImGui::BeginPopup("add")) {
            for (u32 i = 1; i < type_count; ++i) {
                if (!w.get(g_pw.selected, *types[i]) && types[i] != &reflect::type_of<world::PrefabInstance>() && ImGui::MenuItem(types[i]->name)) {
                    w.add(g_pw.selected, *types[i]);
                    g_pw.dirty = true;
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void shutdown_prefab_editor() {
    close_window();
    if (g_pw.device && g_pw.target.valid()) g_pw.device->destroy_texture(g_pw.target);
    g_pw.target = {};
}

} // namespace lsd::editor
