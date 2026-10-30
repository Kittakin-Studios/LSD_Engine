#include "editor_ui.h"

#include "mesh_cache.h"
#include "thumbnails.h"
#include "transactions.h"

#include "lsd/debug_ui/details_panel.h"
#include "lsd/world/components.h"
#include "lsd/world/level.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <ImGuizmo.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace lsd::editor {

namespace {

using world::Entity;

constexpr char kThemePath[] = "Config/editor_theme.json";
constexpr char kContentDir[] = "Content";
constexpr char kLevelsFolder[] = "Content/Levels";
constexpr char kAssetPayload[] = "LSD_ASSET";    // drag-and-drop payload: an asset::Guid
constexpr char kPlacePayload[] = "LSD_PLACE";    // a PlaceItem index
constexpr char kEntityPayload[] = "LSD_ENTITY";  // an Entity (Outliner drag to parent)
constexpr char kOutlinerWindow[] = "Outliner";
constexpr char kDetailsWindow[] = "Details";
constexpr char kContentWindow[] = "Content Browser";

ImU32 u32_color(const LinearColor& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec4 vec4(const LinearColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// Place panel items (Unreal's Place Actors: basic shapes and gameplay helpers).
struct PlaceItem {
    const char* label;
    const char* mesh;  // basic shape, or nullptr
    bool player_start;
};
constexpr PlaceItem kPlaceItems[] = {
    {"Cube", "Cube", false}, {"Sphere", "Sphere", false}, {"Plane", "Plane", false},
    {"Player Start", nullptr, true}, {"Empty", nullptr, false},
};

// UI-only state for the confirmation dialogs.
char g_confirm_open[260] = "";  // level to open after "discard changes?"
bool g_confirm_new = false;
bool g_open_confirm = false;
bool g_open_save_as = false;
char g_save_as_name[96] = "";
bool g_was_using_gizmo = false;
bool g_alt_duplicated = false;

// ---- Edits ----

// Names the edit in progress; it becomes an undo step once the mouse and widgets are idle.
void edited(EditorState& s, const char* label) { std::snprintf(s.pending_edit, sizeof s.pending_edit, "%s", label); }

void select(EditorState& s, Entity e) {
    s.selected = e;
    if (e.valid()) s.selected_asset = {};
}

Entity duplicate(world::World& w, Entity source, Entity parent) {
    char name[64];
    w.unique_name(w.name(source), name, sizeof name);
    const Entity copy = w.create(name);
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = world::component_types(type_count);
    for (u32 t = 0; t < type_count; ++t) {
        if (const void* from = w.get(source, *types[t])) std::memcpy(w.add(copy, *types[t]), from, types[t]->size);
    }
    w.set_parent(copy, parent, false);
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {  // children too (slot order keeps them stable)
        const Entity child = w.entity_at_slot(slot);
        if (child.valid() && child != copy && w.parent(child) == source) duplicate(w, child, copy);
    }
    return copy;
}

Entity place(EditorState& s, const char* name, const char* mesh, bool player_start, Vec3 position) {
    char unique[64];
    s.world->unique_name(name, unique, sizeof unique);
    const Entity e = s.world->create(unique);
    s.world->get<world::Transform>(e)->position = position;
    if (mesh) std::snprintf(s.world->add<world::StaticMesh>(e)->mesh, sizeof(world::StaticMesh::mesh), "%s", mesh);
    if (player_start) s.world->add<world::PlayerStart>(e);
    select(s, e);
    char label[96];
    std::snprintf(label, sizeof label, "Place %s", unique);
    edited(s, label);
    return e;
}

void delete_selected(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    char label[96];
    std::snprintf(label, sizeof label, "Delete %s", s.world->name(s.selected));
    s.world->destroy(s.selected);
    s.selected = {};
    edited(s, label);
}

// World-space box of an entity's mesh; a small box around the origin when it has none.
void world_bounds(EditorState& s, Entity e, Vec3& lo, Vec3& hi) {
    const Mat4 m = s.world->world_matrix(e);
    Vec3 local_lo{-0.25f, -0.25f, 0.0f}, local_hi{0.25f, 0.25f, 1.8f};
    if (const auto* mesh = s.world->get<world::StaticMesh>(e)) {
        if (const CachedMesh* cached = s.meshes->get(mesh->mesh)) {
            local_lo = cached->bounds_min;
            local_hi = cached->bounds_max;
        }
    }
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = transform_point(m, {(c & 1) ? local_hi.x : local_lo.x, (c & 2) ? local_hi.y : local_lo.y, (c & 4) ? local_hi.z : local_lo.z});
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
}

// ---- Picking ----

struct Ray {
    Vec3 origin, dir;
};

Ray mouse_ray(const EditorCamera& cam, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    const Mat4 view = camera_view(cam);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    const Vec3 back{view.at(2, 0), view.at(2, 1), view.at(2, 2)};
    const f32 tan_half = std::tan(cam.fov_y * 0.5f);
    const f32 nx = ((mouse.x - pos.x) / size.x * 2.0f - 1.0f) * tan_half * (size.x / size.y);
    const f32 ny = (1.0f - (mouse.y - pos.y) / size.y * 2.0f) * tan_half;
    return {cam.position, normalize(right * nx + up * ny - back)};
}

// Slab test against a local box; t is in the ray's units.
bool ray_box(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, f32& t) {
    f32 t0 = 0.0f, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        const f32 oa = a == 0 ? o.x : a == 1 ? o.y : o.z, da = a == 0 ? d.x : a == 1 ? d.y : d.z;
        f32 la = a == 0 ? lo.x : a == 1 ? lo.y : lo.z, ha = a == 0 ? hi.x : a == 1 ? hi.y : hi.z;
        if (ha - la < 1e-3f) { la -= 0.01f; ha += 0.01f; }  // flat planes
        if (std::fabs(da) < 1e-9f) {
            if (oa < la || oa > ha) return false;
            continue;
        }
        f32 n = (la - oa) / da, f = (ha - oa) / da;
        if (n > f) { const f32 x = n; n = f; f = x; }
        t0 = n > t0 ? n : t0;
        t1 = f < t1 ? f : t1;
        if (t0 > t1) return false;
    }
    t = t0;
    return true;
}

// Nearest mesh under the ray. shortcut: tests each mesh's bounding box, not its triangles;
// triangle-accurate picking when imported meshes get detailed.
bool raycast(EditorState& s, const Ray& ray, Entity ignore, Entity& hit, f32& hit_t) {
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    hit_t = 1e30f;
    for (u32 i = 0; i < s.world->count(type); ++i) {
        const Entity e = s.world->owner(type, i);
        if (e == ignore) continue;
        const CachedMesh* cached = s.meshes->get(static_cast<const world::StaticMesh*>(s.world->at(type, i))->mesh);
        if (!cached) continue;
        const Mat4 inv = affine_inverse(s.world->world_matrix(e));
        f32 t;
        if (ray_box(transform_point(inv, ray.origin), transform_vector(inv, ray.dir), cached->bounds_min, cached->bounds_max, t) && t < hit_t) {
            hit_t = t;
            hit = e;
        }
    }
    return hit_t < 1e30f;
}

// World point to window pixels; false if behind the camera.
bool project(const Mat4& view_proj, Vec3 p, ImVec2 pos, ImVec2 size, ImVec2& out) {
    const f32 x = view_proj.at(0, 0) * p.x + view_proj.at(0, 1) * p.y + view_proj.at(0, 2) * p.z + view_proj.at(0, 3);
    const f32 y = view_proj.at(1, 0) * p.x + view_proj.at(1, 1) * p.y + view_proj.at(1, 2) * p.z + view_proj.at(1, 3);
    const f32 w = view_proj.at(3, 0) * p.x + view_proj.at(3, 1) * p.y + view_proj.at(3, 2) * p.z + view_proj.at(3, 3);
    if (w < 0.01f) return false;
    out = ImVec2(pos.x + (x / w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - y / w * 0.5f) * size.y);
    return true;
}

// Where a drop or new object lands: on the surface under the mouse, else the floor (z = 0).
Vec3 drop_point(EditorState& s, const Ray& ray) {
    Entity hit;
    f32 t;
    if (raycast(s, ray, {}, hit, t)) return ray.origin + ray.dir * t;
    if (ray.dir.z < -1e-4f) return ray.origin + ray.dir * (-ray.origin.z / ray.dir.z);
    return ray.origin + ray.dir * 10.0f;
}

// End key: drops the selection onto what is below it (or the floor), like Unreal.
void drop_to_floor(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    Vec3 lo, hi;
    world_bounds(s, s.selected, lo, hi);
    const Vec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z};
    Entity hit;
    f32 t;
    f32 floor_z = 0.0f;
    if (raycast(s, {center + Vec3{0, 0, 0.01f}, {0, 0, -1}}, s.selected, hit, t)) floor_z = center.z + 0.01f - t;
    else if (center.z < 0.0f) return;  // below the floor with nothing under it: leave it
    Mat4 m = s.world->world_matrix(s.selected);
    m.at(2, 3) += floor_z - lo.z;
    s.world->set_world_matrix(s.selected, m);
    edited(s, "Drop to Floor");
}

void focus_selected(EditorState& s) {
    if (!s.world->alive(s.selected)) return;
    Vec3 lo, hi;
    world_bounds(s, s.selected, lo, hi);
    const Vec3 center = (lo + hi) * 0.5f;
    const f32 radius = length(hi - lo) * 0.5f;
    s.camera.pivot_distance = radius * 2.5f + 1.0f;
    s.camera.position = center - camera_forward(s.camera) * s.camera.pivot_distance;
}

// ---- Layout ----

void build_default_layout(ImGuiID dockspace, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID right = 0, left = 0, right_top = 0, right_bottom = 0, content = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, &right_top, &right_bottom);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.30f, &content, &viewport);
    ImGui::DockBuilderDockWindow(kOutlinerWindow, right_top);
    ImGui::DockBuilderDockWindow(kDetailsWindow, right_bottom);
    ImGui::DockBuilderDockWindow(kContentWindow, content);
    ImGui::DockBuilderFinish(dockspace);
}

// Panels draw their own themed tab bars, so the dock nodes hide theirs.
bool begin_panel(const char* name) {
    ImGuiWindowClass window_class;
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&window_class);
    return ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoCollapse);
}

// One themed tab. Text turns dark on the active tab so it stays readable on bright colours.
bool themed_tab(const char* label, int index, int& active, const LinearColor& on, const LinearColor& off, const LinearColor& on_text) {
    const bool is_active = active == index;
    ImGui::PushStyleColor(ImGuiCol_Tab, vec4(off));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, vec4(on));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, vec4(on));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, vec4(off));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, vec4(on));
    if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, vec4(on_text));
    const bool open = ImGui::BeginTabItem(label);
    if (is_active) ImGui::PopStyleColor();
    ImGui::PopStyleColor(5);
    if (open) active = index;
    return open;
}

bool toggle_button(const char* label, bool on) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(label);
    if (on) ImGui::PopStyleColor();
    return clicked;
}

// ---- Viewport ----

void update_camera(EditorState& s, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    EditorCamera& cam = s.camera;
    static bool looking = false, panning = false, orbiting = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) looking = true;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning = true;
    if (hovered && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) orbiting = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) looking = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbiting = false;

    const Mat4 view = camera_view(cam);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    auto clamp_pitch = [&] { cam.pitch = cam.pitch < -1.55f ? -1.55f : cam.pitch > 1.55f ? 1.55f : cam.pitch; };

    if (looking) {  // RMB: look around; WASD / QE fly; wheel changes speed (Unreal controls)
        cam.yaw -= io.MouseDelta.x * 0.005f;
        cam.pitch -= io.MouseDelta.y * 0.005f;
        clamp_pitch();
        const Vec3 fwd = camera_forward(cam);
        Vec3 move = fwd * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_W) - ImGui::IsKeyDown(ImGuiKey_S)) +
                    right * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_D) - ImGui::IsKeyDown(ImGuiKey_A)) +
                    Vec3{0, 0, 1} * static_cast<f32>(ImGui::IsKeyDown(ImGuiKey_E) - ImGui::IsKeyDown(ImGuiKey_Q));
        f32& speed = s.settings.camera_speed;
        if (io.MouseWheel != 0.0f) speed = speed * std::pow(1.25f, io.MouseWheel);
        speed = speed < 0.1f ? 0.1f : speed > 100.0f ? 100.0f : speed;
        if (length(move) > 0.0f) cam.position = cam.position + normalize(move) * (speed * (io.KeyShift ? 3.0f : 1.0f) * io.DeltaTime);
    } else if (panning) {
        const f32 k = cam.pivot_distance * 0.0016f;
        cam.position = cam.position - right * (io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    } else if (orbiting) {
        const Vec3 pivot = cam.position + camera_forward(cam) * cam.pivot_distance;
        cam.yaw -= io.MouseDelta.x * 0.005f;
        cam.pitch -= io.MouseDelta.y * 0.005f;
        clamp_pitch();
        cam.position = pivot - camera_forward(cam) * cam.pivot_distance;
    } else if (hovered && io.MouseWheel != 0.0f) {  // dolly toward the pivot
        const f32 step = (cam.pivot_distance > 5.0f ? cam.pivot_distance : 5.0f) * 0.12f * io.MouseWheel;
        cam.position = cam.position + camera_forward(cam) * step;
        cam.pivot_distance = cam.pivot_distance - step > 0.5f ? cam.pivot_distance - step : 0.5f;
    }
}

// Gizmo for the selection (ImGuizmo). Alt+drag duplicates first, like Unreal.
void draw_gizmo(EditorState& s, const Mat4& view, const Mat4& proj, ImVec2 pos, ImVec2 size) {
    if (!s.world->alive(s.selected) || s.game_view || s.playing) {
        g_was_using_gizmo = false;
        return;
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    const ImGuizmo::OPERATION op = s.gizmo == GizmoMode::Translate ? ImGuizmo::TRANSLATE
                                 : s.gizmo == GizmoMode::Rotate    ? ImGuizmo::ROTATE
                                                                   : ImGuizmo::SCALE;
    const f32 step = s.gizmo == GizmoMode::Translate ? s.settings.snap_move : s.gizmo == GizmoMode::Rotate ? s.settings.snap_angle : s.settings.snap_scale;
    const f32 snap[3] = {step, step, step};
    const Mat4 before = s.world->world_matrix(s.selected);
    Mat4 m = before;
    const bool changed = ImGuizmo::Manipulate(view.m, proj.m, op, s.gizmo_local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                              s.settings.snap ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    if (using_now && !g_was_using_gizmo) g_alt_duplicated = false;
    if (changed) {
        if (ImGui::GetIO().KeyAlt && !g_alt_duplicated) {
            const Entity copy = duplicate(*s.world, s.selected, s.world->parent(s.selected));
            s.world->set_world_matrix(s.selected, before);  // the original stays put
            select(s, copy);
            g_alt_duplicated = true;
        }
        s.world->set_world_matrix(s.selected, m);
        edited(s, g_alt_duplicated ? "Duplicate" : s.gizmo == GizmoMode::Translate ? "Move" : s.gizmo == GizmoMode::Rotate ? "Rotate" : "Scale");
    }
    g_was_using_gizmo = using_now;
}

// Icons for entities without a mesh (Player Start, empties), drawn over the 3D view.
void draw_icons(EditorState& s, ImDrawList* dl, const Mat4& view_proj, ImVec2 pos, ImVec2 size) {
    const EditorTheme& t = s.theme;
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (!e.valid() || s.world->get<world::StaticMesh>(e)) continue;
        const Mat4 m = s.world->world_matrix(e);
        const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        ImVec2 base, head, tip;
        const bool start = s.world->get<world::PlayerStart>(e) != nullptr;
        if (!project(view_proj, origin, pos, size, base)) continue;
        const ImU32 color = u32_color(e == s.selected ? t.details_tab_active : start ? t.text_blueprint : t.text_component);
        if (start && project(view_proj, origin + Vec3{0, 0, 1.8f}, pos, size, head)) {
            // A standing capsule from feet to head, plus an arrow for the facing direction.
            const f32 radius = std::fabs(head.y - base.y) * 0.17f + 2.0f;
            dl->AddLine(ImVec2(base.x - radius, base.y), ImVec2(head.x - radius, head.y), color, 2.0f);
            dl->AddLine(ImVec2(base.x + radius, base.y), ImVec2(head.x + radius, head.y), color, 2.0f);
            dl->AddCircle(head, radius, color, 16, 2.0f);
            dl->AddCircle(base, radius, color, 16, 2.0f);
            const Vec3 facing = normalize(transform_vector(m, {1, 0, 0}));
            if (project(view_proj, origin + Vec3{0, 0, 0.9f} + facing * 0.8f, pos, size, tip)) {
                ImVec2 mid;
                project(view_proj, origin + Vec3{0, 0, 0.9f}, pos, size, mid);
                dl->AddLine(mid, tip, color, 2.5f);
                dl->AddCircleFilled(tip, 4.0f, color);
            }
            dl->AddText(ImVec2(head.x + radius + 4, head.y - 8), color, s.world->name(e));
        } else {
            dl->AddCircle(base, 7.0f, color, 12, 2.0f);
            dl->AddText(ImVec2(base.x + 10, base.y - 8), color, s.world->name(e));
        }
    }
}

// Click picking: meshes by their boxes, icons by screen distance.
Entity pick(EditorState& s, const Mat4& view_proj, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    Entity hit;
    f32 t;
    raycast(s, mouse_ray(s.camera, pos, size, mouse), {}, hit, t);
    for (u32 slot = 0; slot < s.world->slot_count(); ++slot) {
        const Entity e = s.world->entity_at_slot(slot);
        if (!e.valid() || s.world->get<world::StaticMesh>(e)) continue;
        const Mat4 m = s.world->world_matrix(e);
        ImVec2 base, head;
        const Vec3 origin{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (!project(view_proj, origin, pos, size, base)) continue;
        if (!project(view_proj, origin + Vec3{0, 0, s.world->get<world::PlayerStart>(e) ? 1.8f : 0.0f}, pos, size, head)) head = base;
        const f32 lo = (head.y < base.y ? head.y : base.y) - 10.0f, hi = (head.y > base.y ? head.y : base.y) + 10.0f;
        const f32 depth = length(origin - s.camera.position);
        if (std::fabs(mouse.x - base.x) < 14.0f && mouse.y > lo && mouse.y < hi && depth < t) {
            hit = e;
            t = depth;
        }
    }
    return hit;
}

void draw_viewport(EditorState& s, ImVec2 pos, ImVec2 size) {
    if (size.x < 8 || size.y < 8) return;
    const EditorTheme& t = s.theme;
    ImGuiIO& io = ImGui::GetIO();
    s.viewport_rect[0] = pos.x;
    s.viewport_rect[1] = pos.y;
    s.viewport_rect[2] = size.x;
    s.viewport_rect[3] = size.y;
    const Mat4 view = camera_view(s.camera);
    const Mat4 proj = camera_projection(s.camera, size.x / size.y);
    const Mat4 view_proj = proj * view;

    // Invisible window covering the viewport: owns its mouse input, the gizmo and asset drops.
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##ViewportInput", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    if (s.playing) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) s.mouse_captured = true;  // click back in to resume control
        ImGui::End();
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(pos.x + 12, pos.y + 10), u32_color(t.details_tab_active),
                                                s.mouse_captured ? "Playing   Esc: stop   Shift+F1: free the mouse"
                                                                 : "Playing (mouse free)   click the viewport to control   Esc: stop");
        return;
    }

    draw_gizmo(s, view, proj, pos, size);
    const bool gizmo_busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !gizmo_busy) select(s, pick(s, view_proj, pos, size, io.MousePos));

    if (ImGui::BeginDragDropTarget()) {
        const Ray ray = mouse_ray(s.camera, pos, size, io.MousePos);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            asset::Guid guid;
            std::memcpy(&guid, payload->Data, sizeof guid);
            const asset::AssetRecord* record = s.registry->find(guid);
            if (record && record->type == asset::AssetType::Mesh) {
                char key[33];
                asset::to_string(guid, key);
                place(s, record->name, key, false, drop_point(s, ray));
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPlacePayload)) {
            const PlaceItem& item = kPlaceItems[*static_cast<const int*>(payload->Data)];
            place(s, item.label, item.mesh, item.player_start, drop_point(s, ray));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
    update_camera(s, hovered);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    if (!s.game_view) draw_icons(s, dl, view_proj, pos, size);
    // Axis gizmo, bottom-left: world axes as seen from the camera.
    if (s.show_axis_gizmo && !s.game_view) {
        const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
        const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
        const ImVec2 origin(pos.x + 48, pos.y + size.y - 48);
        const struct { Vec3 dir; const LinearColor* color; const char* label; } axes[3] = {
            {{1, 0, 0}, &t.axis_x, "X"}, {{0, 1, 0}, &t.axis_y, "Y"}, {{0, 0, 1}, &t.axis_z, "Z"}};
        for (const auto& a : axes) {
            const ImVec2 tip(origin.x + dot(a.dir, right) * 32.0f, origin.y - dot(a.dir, up) * 32.0f);
            dl->AddLine(origin, tip, u32_color(*a.color), 2.5f);
            dl->AddText(ImVec2(tip.x + 3, tip.y - 8), u32_color(*a.color), a.label);
        }
    }
    dl->PopClipRect();

    // Toolbar overlay, top-left of the viewport.
    ImGui::SetNextWindowPos(ImVec2(pos.x + 8, pos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.65f);
    const ImGuiWindowFlags overlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##ViewportToolbar", nullptr, overlay)) {
        ImGui::Button("Perspective");
        ImGui::SetItemTooltip("Orthographic views come later in milestone 3");
        ImGui::SameLine();
        ImGui::Button("Lit");
        ImGui::SetItemTooltip("View modes (Unlit, Wireframe...) arrive with the renderer");
        ImGui::SameLine();
        if (ImGui::Button("Show")) ImGui::OpenPopup("ShowMenu");
        if (ImGui::BeginPopup("ShowMenu")) {
            ImGui::Checkbox("Grid", &s.show_grid);
            ImGui::Checkbox("Axis gizmo", &s.show_axis_gizmo);
            ImGui::Checkbox("Game view (G)", &s.game_view);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (toggle_button("Grid", s.show_grid)) s.show_grid = !s.show_grid;
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (toggle_button("Move", s.gizmo == GizmoMode::Translate)) s.gizmo = GizmoMode::Translate;
        ImGui::SetItemTooltip("W");
        ImGui::SameLine();
        if (toggle_button("Rotate", s.gizmo == GizmoMode::Rotate)) s.gizmo = GizmoMode::Rotate;
        ImGui::SetItemTooltip("E");
        ImGui::SameLine();
        if (toggle_button("Scale", s.gizmo == GizmoMode::Scale)) s.gizmo = GizmoMode::Scale;
        ImGui::SetItemTooltip("R");
        ImGui::SameLine();
        if (ImGui::Button(s.gizmo_local ? "Local" : "World")) s.gizmo_local = !s.gizmo_local;
        ImGui::SetItemTooltip("Gizmo space (Ctrl+`)");
        ImGui::SameLine();
        if (toggle_button("Snap", s.settings.snap)) s.settings.snap = !s.settings.snap;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        f32* step = s.gizmo == GizmoMode::Translate ? &s.settings.snap_move : s.gizmo == GizmoMode::Rotate ? &s.settings.snap_angle : &s.settings.snap_scale;
        ImGui::DragFloat("##snap", step, 0.05f, 0.01f, 360.0f, s.gizmo == GizmoMode::Rotate ? "%.0f deg" : s.gizmo == GizmoMode::Translate ? "%.2f m" : "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##speed", &s.settings.camera_speed, 0.5f, 50.0f, "speed %.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Camera speed (RMB + wheel)");
        ImGui::SameLine();
        ImGui::TextDisabled("RMB: look + WASD/QE fly  MMB: pan  Alt+LMB: orbit  F: focus");
    }
    ImGui::End();
}

// ---- Outliner ----

struct OutlineRow {
    Entity entity;
    int depth;
};

void collect_rows(const world::World& w, Entity parent, int depth, OutlineRow* rows, int& count, int max) {
    // shortcut: scans all slots per level of the tree; keep child lists when levels get big.
    for (u32 slot = 0; slot < w.slot_count() && count < max; ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (!e.valid() || w.parent(e) != parent) continue;
        rows[count++] = {e, depth};
        collect_rows(w, e, depth + 1, rows, count, max);
    }
}

bool contains_ignore_case(const char* text, const char* needle) {
    for (; *text; ++text) {
        const char* a = text;
        const char* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
        if (!*b) return true;
    }
    return !*needle;
}

void draw_rows(EditorState& s, const OutlineRow* rows, int row_count, bool tree) {
    const EditorTheme& t = s.theme;
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, vec4(t.outline_scrollbar));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.outline_row_b));
    ImGui::BeginChild(tree ? "HierarchyRows" : "OutlineRows", ImVec2(0, 0), ImGuiChildFlags_None);
    const f32 row_height = ImGui::GetTextLineHeight() + 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 left = ImGui::GetWindowPos().x;
    const f32 right = left + ImGui::GetWindowWidth();
    int visible = static_cast<int>(ImGui::GetWindowHeight() / row_height) + 1;
    visible = visible > row_count ? visible : row_count;
    for (int i = 0; i < visible; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(left, p.y), ImVec2(right, p.y + row_height), u32_color((i & 1) ? t.outline_row_b : t.outline_row_a));
        if (i >= row_count) { ImGui::Dummy(ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y)); continue; }
        const OutlineRow& row = rows[i];
        const Entity e = row.entity;
        const LinearColor& text = s.world->get<world::StaticMesh>(e) ? t.text_mesh : s.world->get<world::PlayerStart>(e) ? t.text_blueprint : t.text_component;
        const int depth = tree ? row.depth : 0;
        if (depth) {  // child hook drawn as lines: the UI font has no arrow glyph
            const f32 hx = p.x + static_cast<f32>(depth) * 22.0f - 14.0f;
            const f32 mid = p.y + row_height * 0.5f;
            dl->AddLine(ImVec2(hx, p.y + 4.0f), ImVec2(hx, mid), u32_color(text), 1.5f);
            dl->AddLine(ImVec2(hx, mid), ImVec2(hx + 9.0f, mid), u32_color(text), 1.5f);
        }
        ImGui::PushID(static_cast<int>(e.value));
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(text));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<f32>(depth) * 22.0f);
        if (ImGui::Selectable(s.world->name(e), s.selected == e, static_cast<ImGuiSelectableFlags>(ImGuiSelectableFlags_SpanAvailWidth) | ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y))) {
            select(s, e);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focus_selected(s);
        }
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityPayload, &e, sizeof e);
            ImGui::Text("Attach %s to...", s.world->name(e));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                Entity child;
                std::memcpy(&child, payload->Data, sizeof child);
                if (child != e && s.world->set_parent(child, e)) edited(s, "Attach");
                else std::snprintf(s.status, sizeof s.status, "Cannot attach an object to itself or its own child");
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("row_menu")) {
            select(s, e);
            if (ImGui::MenuItem("Focus", "F")) focus_selected(s);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                select(s, duplicate(*s.world, e, s.world->parent(e)));
                edited(s, "Duplicate");
            }
            if (ImGui::MenuItem("Detach", nullptr, false, s.world->parent(e).valid())) {
                s.world->set_parent(e, {});
                edited(s, "Detach");
            }
            if (ImGui::MenuItem("Delete", "Delete")) delete_selected(s);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    // Dropping on the empty area below the rows detaches.
    if (tree && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            Entity child;
            std::memcpy(&child, payload->Data, sizeof child);
            if (s.world->parent(child).valid() && s.world->set_parent(child, {})) edited(s, "Detach");
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopStyleColor(2);
}

void draw_outliner(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 1;
    static char filter[64] = "";
    static OutlineRow rows[4096];
    if (!begin_panel(kOutlinerWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("OutlinerTabs")) {
        if (themed_tab("Outline", 0, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "Search objects", filter, sizeof filter);
            int count = 0;
            for (u32 slot = 0; slot < s.world->slot_count() && count < 4096; ++slot) {
                const Entity e = s.world->entity_at_slot(slot);
                if (e.valid() && contains_ignore_case(s.world->name(e), filter)) rows[count++] = {e, 0};
            }
            draw_rows(s, rows, count, false);
            ImGui::EndTabItem();
        }
        if (themed_tab("Hierarchy", 1, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            int count = 0;
            collect_rows(*s.world, {}, 0, rows, count, 4096);
            draw_rows(s, rows, count, true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Details ----

// Mesh slot for a StaticMesh: basic shapes and mesh assets in a combo; also accepts a dragged asset.
void draw_mesh_slot(EditorState& s, world::StaticMesh& mesh) {
    const char* shown = mesh.mesh;
    asset::Guid guid;
    if (asset::parse_guid(mesh.mesh, guid)) {
        const asset::AssetRecord* r = s.registry->find(guid);
        shown = r ? r->name : "(missing asset)";
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mesh");
    ImGui::SameLine(160.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##mesh_slot", shown)) {
        for (const char* shape : {"Cube", "Sphere", "Plane"}) {
            if (ImGui::Selectable(shape, std::strcmp(mesh.mesh, shape) == 0)) {
                std::snprintf(mesh.mesh, sizeof mesh.mesh, "%s", shape);
                edited(s, "Change Mesh");
            }
        }
        ImGui::Separator();
        for (usize i = 0; i < s.registry->count(); ++i) {
            const asset::AssetRecord& r = s.registry->get(i);
            if (r.type != asset::AssetType::Mesh) continue;
            char key[33];
            asset::to_string(r.guid, key);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(r.name, std::strcmp(mesh.mesh, key) == 0)) {
                std::snprintf(mesh.mesh, sizeof mesh.mesh, "%s", key);
                edited(s, "Change Mesh");
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            std::memcpy(&guid, payload->Data, sizeof guid);
            const asset::AssetRecord* r = s.registry->find(guid);
            if (r && r->type == asset::AssetType::Mesh) {
                asset::to_string(guid, mesh.mesh);
                edited(s, "Change Mesh");
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_entity_details(EditorState& s) {
    EditorTheme& t = s.theme;
    world::World& w = *s.world;
    const Entity e = s.selected;
    char name[64];
    std::snprintf(name, sizeof name, "%s", w.name(e));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", name, sizeof name)) {
        w.set_name(e, name);
        edited(s, "Rename");
    }
    if (const Entity parent = w.parent(e); parent.valid()) ImGui::TextDisabled("Attached to %s", w.name(parent));

    ImGui::BeginChild("DetailsRows");
    const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
    u32 type_count = 0;
    const reflect::TypeInfo* const* types = world::component_types(type_count);
    for (u32 i = 0; i < type_count; ++i) {
        void* component = w.get(e, *types[i]);
        if (!component) continue;
        ImGui::PushID(types[i]->name);
        bool keep = true;
        if (ImGui::CollapsingHeader(types[i]->name, i == 0 ? nullptr : &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (types[i] == &reflect::type_of<world::StaticMesh>()) draw_mesh_slot(s, *static_cast<world::StaticMesh*>(component));
            if (draw_properties(*types[i], component, &style)) {
                char label[96];
                std::snprintf(label, sizeof label, "Edit %s", types[i]->name);
                edited(s, label);
            }
        }
        if (!keep) {
            w.remove(e, *types[i]);
            edited(s, "Remove Component");
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("+ Add Component", ImVec2(-1.0f, 0))) ImGui::OpenPopup("AddComponent");
    if (ImGui::BeginPopup("AddComponent")) {
        for (u32 i = 1; i < type_count; ++i) {
            if (!w.get(e, *types[i]) && ImGui::MenuItem(types[i]->name)) {
                w.add(e, *types[i]);
                edited(s, "Add Component");
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

void draw_details_panel(EditorState& s) {
    EditorTheme& t = s.theme;
    if (!begin_panel(kDetailsWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("DetailsTabs")) {
        const asset::AssetRecord* selected = s.registry && s.selected_asset.valid() ? s.registry->find(s.selected_asset) : nullptr;
        if (themed_tab("Details", 0, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
            if (s.world->alive(s.selected)) {
                draw_entity_details(s);
            } else if (selected) {
                ImGui::Text("%s", selected->name);
                ImGui::TextDisabled("%s  |  Content/%s", asset::to_string(selected->type), selected->path);
                if (ImGui::Button("Save")) {
                    // The data compiler sees the new settings and rebuilds; placed copies update live.
                    std::snprintf(s.status, sizeof s.status, s.registry->save_meta(*selected, s.selected_meta) ? "Saved %s.meta" : "Could not save %s.meta", selected->name);
                }
                ImGui::SameLine();
                if (ImGui::Button("Revert")) s.registry->load_meta(*selected, s.selected_meta);
                if (s.status[0]) ImGui::TextDisabled("%s", s.status);
                ImGui::BeginChild("DetailsRows");
                const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
                draw_details(reflect::type_of<asset::AssetMeta>(), &s.selected_meta, &style);
                ImGui::EndChild();
            } else {
                ImGui::TextDisabled("Select an object in the viewport or the Outliner,\nor an asset in the Content Browser.");
            }
            ImGui::EndTabItem();
        }
        if (themed_tab("World Partition", 1, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
            ImGui::TextDisabled("World Partition (level streaming grid) arrives in a later milestone.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Content Browser ----

std::string join_folder(const char* parent, const char* child) {
    return parent[0] ? std::string(parent) + "/" + child : std::string(child);
}

// Accepts an asset dragged onto the last item (folder tile or tree node) and moves it there.
void folder_drop_target(EditorState& s, const char* folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
        asset::Guid guid;
        std::memcpy(&guid, payload->Data, sizeof guid);
        char error[200] = "";
        if (const asset::AssetRecord* r = s.registry->find(guid)) {
            if (std::strcmp(r->folder, folder) == 0) {
                // already there
            } else if (s.registry->move_asset(*r, folder, error, sizeof error)) {
                std::snprintf(s.status, sizeof s.status, "Moved to Content/%s", folder);
            } else {
                std::snprintf(s.status, sizeof s.status, "Move failed: %s", error);
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void show_in_explorer(const char* folder) {
    std::error_code error;
    const std::string path = std::filesystem::absolute(std::filesystem::path(kContentDir) / folder, error).generic_string();
    SDL_OpenURL(("file:///" + path).c_str());
}

void start_rename(EditorState& s, asset::Guid guid, const char* folder, const char* current) {
    s.rename_asset = guid;
    std::snprintf(s.rename_folder, sizeof s.rename_folder, "%s", folder);
    std::snprintf(s.rename_buffer, sizeof s.rename_buffer, "%s", current);
    s.open_rename = true;
}

// Opens a level, asking first if the current one has unsaved changes.
void request_open(EditorState& s, const char* path) {
    if (s.history->dirty()) {
        std::snprintf(g_confirm_open, sizeof g_confirm_open, "%s", path);
        g_confirm_new = false;
        g_open_confirm = true;
    } else {
        std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", path);
    }
}

void request_new(EditorState& s) {
    if (s.history->dirty()) {
        g_confirm_open[0] = '\0';
        g_confirm_new = true;
        g_open_confirm = true;
    } else {
        s.new_level_requested = true;
    }
}

void request_save(EditorState& s, bool save_as) {
    if (!save_as && s.level_path[0]) {
        s.save_requested = true;
        return;
    }
    std::snprintf(g_save_as_name, sizeof g_save_as_name, "%s", s.level_path[0] ? std::filesystem::path(s.level_path).stem().string().c_str() : "NewLevel");
    g_open_save_as = true;
}

// Rename and delete confirmation dialogs; actions happen on OK.
void draw_file_dialogs(EditorState& s) {
    if (s.open_rename) ImGui::OpenPopup("Rename");
    if (s.open_delete) ImGui::OpenPopup("Delete");
    s.open_rename = s.open_delete = false;

    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", s.rename_buffer, sizeof s.rename_buffer, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = asset::valid_name(s.rename_buffer);
        if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not a valid name");
        if ((ImGui::Button("Rename") || enter) && ok) {
            char error[200] = "";
            bool done = false;
            if (const asset::AssetRecord* r = s.rename_asset.valid() ? s.registry->find(s.rename_asset) : nullptr) {
                char old_path[300];
                s.registry->source_path(*r, old_path, sizeof old_path);
                done = s.registry->rename_asset(*r, s.rename_buffer, error, sizeof error);
                // Renaming the open level: keep saving to the new file.
                if (done && std::filesystem::path(old_path) == std::filesystem::path(s.level_path)) {
                    if (const asset::AssetRecord* renamed = s.registry->find(s.rename_asset)) s.registry->source_path(*renamed, s.level_path, sizeof s.level_path);
                }
            } else if (s.rename_folder[0]) {
                const std::filesystem::path from = std::filesystem::path(kContentDir) / s.rename_folder;
                const std::filesystem::path to = from.parent_path() / s.rename_buffer;
                std::error_code ec;
                if (std::filesystem::exists(to, ec)) std::snprintf(error, sizeof error, "%s already exists", s.rename_buffer);
                else std::filesystem::rename(from, to, ec);
                if (ec) std::snprintf(error, sizeof error, "%s", ec.message().c_str());
                done = !error[0];
                if (done) {
                    s.registry->scan();  // assets inside keep their GUIDs: their .meta files moved too
                    s.content_folder[0] = '\0';
                }
            }
            std::snprintf(s.status, sizeof s.status, done ? "Renamed to %s" : "Rename failed: %s", done ? s.rename_buffer : error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const asset::AssetRecord* r = s.delete_asset.valid() ? s.registry->find(s.delete_asset) : nullptr;
        if (r) ImGui::Text("Delete Content/%s?\nObjects using it in levels will show nothing.", r->path);
        else ImGui::Text("Delete the empty folder Content/%s?", s.delete_folder);
        if (ImGui::Button("Delete")) {
            char error[200] = "";
            bool done = false;
            if (r) {
                const std::string name = r->name;
                done = s.registry->delete_asset(*r, error, sizeof error);
                if (done) std::snprintf(error, sizeof error, "%s", name.c_str());
                if (s.selected_asset == s.delete_asset) s.selected_asset = {};
            } else {
                std::error_code ec;
                done = std::filesystem::remove(std::filesystem::path(kContentDir) / s.delete_folder, ec);  // fails unless empty
                std::snprintf(error, sizeof error, "%s", done ? s.delete_folder : "folder is not empty");
            }
            std::snprintf(s.status, sizeof s.status, done ? "Deleted %s" : "Delete failed: %s", error);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Draws the sub-folders of `dir` as tree nodes; clicking one selects it. Returns false if none.
// shortcut: reads the disk every frame; move to the registry's index when folders get many.
bool draw_folder_tree(EditorState& s, const std::filesystem::path& dir, ImGuiTreeNodeFlags flags) {
    std::error_code error;
    bool any = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (!entry.is_directory(error)) continue;
        any = true;
        const std::string name = entry.path().filename().string();
        const std::string relative = std::filesystem::relative(entry.path(), kContentDir, error).generic_string();
        const bool selected = relative == s.content_folder;
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags | (selected ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", relative.c_str());
            s.search[0] = '\0';
        }
        folder_drop_target(s, relative.c_str());
        if (open) {
            draw_folder_tree(s, entry.path(), flags);
            ImGui::TreePop();
        }
    }
    return any;
}

// Asset tiles for the current folder or search; meshes drag into the viewport, levels open on double-click.
void draw_asset_grid(EditorState& s) {
    const EditorTheme& t = s.theme;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##search", "Search assets", s.search, sizeof s.search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("##type", &s.type_filter, "All types\0Meshes\0Levels\0");
    ImGui::SameLine();
    ImGui::TextDisabled(s.search[0] ? "Results in all folders" : "Content/%s", s.content_folder);
    ImGui::Separator();
    if (!s.registry) return;

    static usize hits[1024];
    const asset::AssetType type = s.type_filter == 1 ? asset::AssetType::Mesh : s.type_filter == 2 ? asset::AssetType::Level : asset::AssetType::Unknown;
    const usize count = s.registry->search(s.search, type, s.content_folder, hits, 1024);

    // Sub-folders first (not while searching), like Unreal; double-click opens one.
    // shortcut: listed from disk every frame, like the folder tree.
    static char folders[64][96];
    usize folder_count = 0;
    if (!s.search[0]) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(kContentDir) / s.content_folder, error)) {
            if (folder_count < 64 && entry.is_directory(error)) {
                std::snprintf(folders[folder_count++], sizeof folders[0], "%s", entry.path().filename().string().c_str());
            }
        }
    }
    // Right-click on empty space: New Folder.
    if (ImGui::BeginPopupContextWindow("grid_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("New Folder")) {
            std::error_code error;
            const std::filesystem::path base = std::filesystem::path(kContentDir) / s.content_folder;
            std::filesystem::path dir = base / "NewFolder";
            for (int n = 2; std::filesystem::exists(dir, error); ++n) dir = base / ("NewFolder" + std::to_string(n));
            std::filesystem::create_directories(dir, error);
            start_rename(s, {}, join_folder(s.content_folder, dir.filename().string().c_str()).c_str(), dir.filename().string().c_str());
        }
        if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(s.content_folder);
        ImGui::EndPopup();
    }
    if (s.selected_asset.valid() && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        if (const asset::AssetRecord* r = s.registry->find(s.selected_asset)) start_rename(s, r->guid, "", r->name);
    }
    draw_file_dialogs(s);

    if (count == 0 && folder_count == 0) {
        ImGui::TextDisabled(s.search[0] ? "No assets match." : "No assets here. Drag .fbx, .gltf or .glb files from Explorer onto the editor, or right-click for New Folder.");
        return;
    }

    constexpr f32 kTile = 96.0f;
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing)) > 0
                            ? static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing))
                            : 1;
    usize tile = 0;
    for (usize f = 0; f < folder_count; ++f, ++tile) {
        ImGui::PushID(folders[f]);
        if (tile % static_cast<usize>(columns) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##folder", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing()));
        const std::string folder_path = join_folder(s.content_folder, folders[f]);
        folder_drop_target(s, folder_path.c_str());
        if (ImGui::BeginPopupContextItem("folder_menu")) {
            if (ImGui::MenuItem("Rename")) start_rename(s, {}, folder_path.c_str(), folders[f]);
            if (ImGui::MenuItem("Delete...")) {
                s.delete_asset = {};
                std::snprintf(s.delete_folder, sizeof s.delete_folder, "%s", folder_path.c_str());
                s.open_delete = true;
            }
            if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(folder_path.c_str());
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", folder_path.c_str());
        }
        // Folder shape: tab on top of a body, in the tree's folder colours.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p.x + 12, p.y + 22), ImVec2(p.x + 46, p.y + 32), u32_color(t.folder_tree_text), 3.0f);
        dl->AddRectFilled(ImVec2(p.x + 12, p.y + 30), ImVec2(p.x + kTile - 12, p.y + kTile - 18), u32_color(t.folder_tree_text), 3.0f);
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + kTile, p.y + kTile + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), folders[f]);
        dl->PopClipRect();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    for (usize n = 0; n < count; ++n, ++tile) {
        const asset::AssetRecord& r = s.registry->get(hits[n]);
        const bool level = r.type == asset::AssetType::Level;
        ImGui::PushID(static_cast<int>(hits[n]));
        if (tile % static_cast<usize>(columns) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = r.guid == s.selected_asset;
        if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing()))) {
            s.selected_asset = r.guid;
            s.selected = {};
            s.selected_meta = asset::AssetMeta{};
            s.registry->load_meta(r, s.selected_meta);
            s.status[0] = '\0';
            if (level && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                char path[300];
                s.registry->source_path(r, path, sizeof path);
                request_open(s, path);
            }
        }
        if (ImGui::BeginPopupContextItem("asset_menu")) {
            if (level && ImGui::MenuItem("Open Level")) {
                char path[300];
                s.registry->source_path(r, path, sizeof path);
                request_open(s, path);
            }
            if (ImGui::MenuItem("Rename", "F2")) start_rename(s, r.guid, "", r.name);
            if (ImGui::MenuItem("Delete...")) {
                s.delete_asset = r.guid;
                s.open_delete = true;
            }
            if (ImGui::MenuItem("Show in Explorer")) show_in_explorer(r.folder);
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kAssetPayload, &r.guid, sizeof r.guid);
            ImGui::Text(level ? "%s (drop on a folder to move it)" : "Place %s", r.name);
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(level ? "%s\nContent/%s\nDouble-click to open" : "%s\nContent/%s", r.name, r.path);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 lo(p.x + 6, p.y + 6), hi(p.x + kTile - 6, p.y + kTile - 6);
        const u64 thumbnail = s.thumbnails && !level ? s.thumbnails->get(r.guid) : 0;
        if (thumbnail) {
            const bool flip = s.thumbnails->flipped();
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(thumbnail)), lo, hi, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
        } else {  // still rendering (or no thumbnail for this type): type badge
            dl->AddRectFilled(lo, hi, u32_color(level ? t.details_tab : t.outline_row_b), 4.0f);
            const char* badge = asset::to_string(r.type);
            const ImVec2 badge_size = ImGui::CalcTextSize(badge);
            dl->AddText(ImVec2(p.x + (kTile - badge_size.x) * 0.5f, p.y + (kTile - badge_size.y) * 0.5f), u32_color(level ? t.text_on_bright : t.text_mesh), badge);
        }
        dl->AddRect(lo, hi, u32_color(selected ? t.details_tab_active : t.text_mesh), 4.0f, 0, 2.0f);
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + kTile, p.y + kTile + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), r.name);
        dl->PopClipRect();
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

// Place tab: basic shapes and gameplay helpers, dragged into the viewport or double-clicked.
void draw_place_panel(EditorState& s) {
    const EditorTheme& t = s.theme;
    constexpr f32 kTile = 96.0f;
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const int fit = static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (kTile + spacing));
    const int columns = fit > 0 ? fit : 1;
    for (int i = 0; i < static_cast<int>(sizeof kPlaceItems / sizeof kPlaceItems[0]); ++i) {
        const PlaceItem& item = kPlaceItems[i];
        ImGui::PushID(i);
        if (i % columns != 0) ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##place", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing())) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const Vec3 ahead = s.camera.position + camera_forward(s.camera) * s.camera.pivot_distance;
            place(s, item.label, item.mesh, item.player_start, {ahead.x, ahead.y, 0.0f});
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kPlacePayload, &i, sizeof i);
            ImGui::Text("Place %s", item.label);
            ImGui::EndDragDropSource();
        }
        ImGui::SetItemTooltip("Drag into the viewport, or double-click to place in front of the camera");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + kTile * 0.5f, p.y + kTile * 0.5f);
        const ImU32 color = u32_color(item.mesh ? t.text_mesh : item.player_start ? t.text_blueprint : t.text_component);
        dl->AddRect(ImVec2(p.x + 6, p.y + 6), ImVec2(p.x + kTile - 6, p.y + kTile - 6), color, 4.0f, 0, 2.0f);
        if (i == 0) dl->AddRect(ImVec2(c.x - 18, c.y - 18), ImVec2(c.x + 18, c.y + 18), color, 0.0f, 0, 2.5f);
        else if (i == 1) dl->AddCircle(c, 20.0f, color, 24, 2.5f);
        else if (i == 2) dl->AddQuad(ImVec2(c.x - 26, c.y + 10), ImVec2(c.x - 8, c.y - 12), ImVec2(c.x + 26, c.y - 12), ImVec2(c.x + 8, c.y + 10), color, 2.5f);
        else if (item.player_start) {
            dl->AddCircle(ImVec2(c.x, c.y - 16), 7.0f, color, 12, 2.5f);
            dl->AddLine(ImVec2(c.x - 7, c.y - 8), ImVec2(c.x - 7, c.y + 22), color, 2.5f);
            dl->AddLine(ImVec2(c.x + 7, c.y - 8), ImVec2(c.x + 7, c.y + 22), color, 2.5f);
        } else {
            dl->AddCircle(c, 8.0f, color, 12, 2.5f);
        }
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), item.label);
        ImGui::PopID();
    }
}

void draw_content_browser(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    const bool open = begin_panel(kContentWindow);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    // Blue bar behind the tab row.
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + ImGui::GetFrameHeight() + 8.0f), u32_color(t.content_bar));
    if (ImGui::BeginTabBar("ContentTabs")) {
        if (themed_tab("Content", 0, active_tab, t.content_tab, t.content_bar, t.text_on_bright)) {
            // Folder tree: black, lime border, yellow text and lines.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.folder_tree_background));
            ImGui::PushStyleColor(ImGuiCol_Border, vec4(t.folder_tree_border));
            ImGui::PushStyleColor(ImGuiCol_TreeLines, vec4(t.folder_tree_text));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
            ImGui::BeginChild("FolderTree", ImVec2(200, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
            const ImGuiTreeNodeFlags node = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_DrawLinesToNodes | ImGuiTreeNodeFlags_SpanAvailWidth;
            ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.folder_tree_border));
            const bool root = ImGui::TreeNodeEx("Content", node | (s.content_folder[0] ? 0 : ImGuiTreeNodeFlags_Selected));
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                s.content_folder[0] = '\0';
                s.search[0] = '\0';
            }
            folder_drop_target(s, "");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.folder_tree_text));
            if (root) {
                if (!draw_folder_tree(s, kContentDir, node)) ImGui::TextDisabled("(no folders)");
                ImGui::TreePop();
            }
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
            ImGui::BeginChild("Assets", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_asset_grid(s);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        if (themed_tab("Place", 1, active_tab, t.content_tab, t.content_bar, t.text_on_bright)) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
            ImGui::BeginChild("PlaceItems", ImVec2(0, 0), ImGuiChildFlags_None);
            draw_place_panel(s);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Menus and windows ----

void draw_menu_bar(EditorState& s) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Level", "Ctrl+N", false, !s.playing)) request_new(s);
        if (ImGui::BeginMenu("Open Level", !s.playing)) {
            bool any = false;
            for (usize i = 0; i < s.registry->count(); ++i) {
                const asset::AssetRecord& r = s.registry->get(i);
                if (r.type != asset::AssetType::Level) continue;
                any = true;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(r.name, r.folder)) {
                    char path[300];
                    s.registry->source_path(r, path, sizeof path);
                    request_open(s, path);
                }
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("No levels yet: File > Save Level As");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Level", "Ctrl+S", false, !s.playing)) request_save(s, false);
        if (ImGui::MenuItem("Save Level As...", "Ctrl+Shift+S", false, !s.playing)) request_save(s, true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, s.history->can_undo() && !s.playing)) s.history->undo(*s.world);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, s.history->can_redo() && !s.playing)) s.history->redo(*s.world);
        ImGui::Separator();
        const bool has = s.world->alive(s.selected) && !s.playing;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has)) {
            select(s, duplicate(*s.world, s.selected, s.world->parent(s.selected)));
            edited(s, "Duplicate");
        }
        if (ImGui::MenuItem("Delete", "Delete", false, has)) delete_selected(s);
        if (ImGui::MenuItem("Drop to Floor", "End", false, has)) drop_to_floor(s);
        ImGui::Separator();
        ImGui::MenuItem("Editor Preferences", nullptr, &s.show_preferences);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Undo History", nullptr, &s.show_history);
        ImGui::EndMenu();
    }
    // Play / Stop, like Unreal's main toolbar.
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 40.0f);
    if (!s.playing) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_y));
        if (ImGui::MenuItem("Play", "Alt+P")) s.play_requested = true;
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, vec4(s.theme.axis_x));
        if (ImGui::MenuItem("Stop", "Esc")) s.stop_requested = true;
        ImGui::PopStyleColor();
    }
    const char* level = s.level_path[0] ? s.level_path : "Untitled level";
    char right[320];
    std::snprintf(right, sizeof right, "%s%s   %s", level, s.history->dirty() ? " *" : "", s.status);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 16.0f);
    ImGui::TextDisabled("%s", right);
    ImGui::EndMainMenuBar();
}

void draw_level_dialogs(EditorState& s) {
    if (s.quit_requested) ImGui::OpenPopup("Quit");
    s.quit_requested = false;
    if (ImGui::BeginPopupModal("Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before closing?", s.level_path[0] ? s.level_path : "the untitled level");
        if (ImGui::Button("Save")) {
            s.quit_after_save = true;
            request_save(s, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            s.quit_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_open_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (g_open_save_as) ImGui::OpenPopup("Save Level As");
    g_open_confirm = g_open_save_as = false;

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The current level has unsaved changes. Discard them?");
        if (ImGui::Button("Discard")) {
            if (g_confirm_new) s.new_level_requested = true;
            else std::snprintf(s.open_level_requested, sizeof s.open_level_requested, "%s", g_confirm_open);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Level As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saved to %s/<name>%s", kLevelsFolder, world::kLevelExtension);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##level_name", g_save_as_name, sizeof g_save_as_name, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = asset::valid_name(g_save_as_name);
        if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not a valid name");
        if ((ImGui::Button("Save") || enter) && ok) {
            std::snprintf(s.save_as_requested, sizeof s.save_as_requested, "%s/%s%s", kLevelsFolder, g_save_as_name, world::kLevelExtension);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s.quit_after_save = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_history_window(EditorState& s) {
    if (!s.show_history) return;
    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Undo History", &s.show_history)) {
        for (u32 i = 0; i < s.history->count(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool current = i == s.history->current();
            if (i > s.history->current()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(s.history->label(i), current) && !s.playing) s.history->jump_to(*s.world, i);
            if (i > s.history->current()) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void draw_preferences_window(EditorState& s) {
    if (!s.show_preferences) return;
    EditorTheme& t = s.theme;
    ImGui::SetNextWindowSize(ImVec2(460, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Editor Preferences", &s.show_preferences)) {
        const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) draw_details(reflect::type_of<EditorSettings>(), &s.settings, &style);
        if (ImGui::CollapsingHeader("Theme", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s", kThemePath);
            if (ImGui::Button("Save Theme")) std::snprintf(s.status, sizeof s.status, reflect::save_file(t, kThemePath) ? "Saved %s" : "Could not save %s", kThemePath);
            ImGui::SameLine();
            if (ImGui::Button("Reload")) {
                reflect::load_file(t, kThemePath);
                apply_theme(t);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                t = EditorTheme{};
                apply_theme(t);
            }
            if (draw_details(reflect::type_of<EditorTheme>(), &t, &style)) apply_theme(t);
        }
    }
    ImGui::End();
}

void handle_shortcuts(EditorState& s) {
    ImGuiIO& io = ImGui::GetIO();
    if (s.playing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s.stop_requested = true;
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F1)) s.mouse_captured = false;
        return;
    }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P)) s.play_requested = true;
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);  // WASD/QE move the camera then
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (io.KeyShift) s.history->redo(*s.world);
        else s.history->undo(*s.world);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) s.history->redo(*s.world);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) request_save(s, io.KeyShift);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) request_new(s);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) s.gizmo_local = !s.gizmo_local;
    const bool has = s.world->alive(s.selected);
    if (ctrl && has && ImGui::IsKeyPressed(ImGuiKey_D)) {
        select(s, duplicate(*s.world, s.selected, s.world->parent(s.selected)));
        edited(s, "Duplicate");
    }
    if (ctrl || flying) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) s.gizmo = GizmoMode::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) s.gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) s.gizmo = GizmoMode::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_G)) s.game_view = !s.game_view;
    if (has && ImGui::IsKeyPressed(ImGuiKey_F)) focus_selected(s);
    if (has && ImGui::IsKeyPressed(ImGuiKey_End)) drop_to_floor(s);
    if (has && ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_selected(s);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s.selected = {};
}

} // namespace

void apply_theme(const EditorTheme& t) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.FontSizeBase = t.font_size;
    style.WindowRounding = 0.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 0.0f;
    style.TreeLinesSize = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = vec4(t.panel_background);
    style.Colors[ImGuiCol_ChildBg] = vec4(t.panel_background);
    style.Colors[ImGuiCol_DockingEmptyBg] = vec4(t.viewport_background);
    style.Colors[ImGuiCol_ScrollbarGrab] = vec4(t.outline_scrollbar);
    style.Colors[ImGuiCol_Header] = vec4(t.details_tab);
    style.Colors[ImGuiCol_HeaderHovered] = vec4(t.details_row_a);
    style.Colors[ImGuiCol_CheckMark] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SliderGrab] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SliderGrabActive] = vec4(t.content_tab);
    style.Colors[ImGuiCol_SeparatorHovered] = vec4(t.details_tab_active);
    style.Colors[ImGuiCol_SeparatorActive] = vec4(t.content_tab);
    style.Colors[ImGuiCol_DockingPreview] = vec4(t.details_tab_active);
}

void draw_editor(EditorState& s) {
    ImGuizmo::BeginFrame();
    draw_menu_bar(s);
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImHashStr("EditorDockspace");
    const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspace);
    if (!existing || !existing->IsSplitNode()) build_default_layout(dockspace, main->WorkSize);
    ImGui::DockSpaceOverViewport(dockspace, main, ImGuiDockNodeFlags_PassthruCentralNode);

    draw_outliner(s);
    draw_details_panel(s);
    draw_content_browser(s);
    draw_history_window(s);
    draw_preferences_window(s);
    draw_level_dialogs(s);
    handle_shortcuts(s);

    // The central node is left empty and transparent: the 3D viewport shows through it.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) draw_viewport(s, central->Pos, central->Size);

    if (!s.world->alive(s.selected)) s.selected = {};  // undo or delete removed it
    // One undo step per finished edit: wait until drags and text fields are released.
    if (s.pending_edit[0] && (s.playing || (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))) {
        if (!s.playing) s.history->commit(*s.world, s.pending_edit);  // Play changes are thrown away on Stop
        s.pending_edit[0] = '\0';
    }
}

Vec3 camera_forward(const EditorCamera& cam) {
    const f32 cp = std::cos(cam.pitch);
    return {cp * std::cos(cam.yaw), cp * std::sin(cam.yaw), std::sin(cam.pitch)};
}

Mat4 camera_view(const EditorCamera& cam) { return look_at(cam.position, cam.position + camera_forward(cam), Vec3{0, 0, 1}); }

Mat4 camera_projection(const EditorCamera& cam, f32 aspect) { return perspective(cam.fov_y, aspect, 0.05f, 1000.0f); }

void load_theme(EditorTheme& theme) { reflect::load_file(theme, kThemePath); }

void make_default_level(world::World& w) {
    w.clear();
    const Entity floor = w.create("Floor");
    world::Transform* t = w.get<world::Transform>(floor);
    t->position = {0, 0, -0.2f};
    t->scale = {20, 20, 0.2f};
    w.add<world::StaticMesh>(floor)->color = LinearColor::hex(0x8A8A8E);
    w.add<world::PlayerStart>(w.create("PlayerStart"));
}

} // namespace lsd::editor
