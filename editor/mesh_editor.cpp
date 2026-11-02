#include "mesh_editor.h"

#include "mesh_cache.h"
#include "skinned_cache.h"

#include "lsd/animation/animation.h"
#include "lsd/asset/asset_meta.h"
#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace lsd::editor {

namespace {

constexpr u32 kSize = 512;

struct MeshWindow {
    bool open = false;
    char name[96] = "";
    char key[33] = "";  // the mesh GUID
    asset::AssetRecord record;
    asset::AssetMeta meta;
    bool meta_dirty = false;
    f32 yaw = -2.4f, pitch = 0.45f, zoom = 1.0f;
    i32 bone = -1;
    rhi::TextureHandle target;
    rhi::Device* device = nullptr;
    EditorState* state = nullptr;
    anim::Pose pose;
    Mat4 view_proj;
    char morph[64] = "";      // the morph target previewed, at morph_weight
    f32 morph_weight = 1.0f;
    rhi::BufferHandle morphed;
    u64 morphed_for = 0;      // what `morphed` was built from (0 = rebuild)
};
MeshWindow g_mw;

bool bounds(EditorState& s, Vec3& lo, Vec3& hi, const SkinnedEntry*& skinned, const CachedMesh*& mesh) {
    skinned = s.skinned ? s.skinned->get(g_mw.key) : nullptr;
    mesh = nullptr;
    if (skinned && skinned->model && skinned->model->joint_count() > 0) {
        lo = skinned->model->bounds_min;
        hi = skinned->model->bounds_max;
        return true;
    }
    skinned = nullptr;
    mesh = s.meshes ? s.meshes->get(g_mw.key) : nullptr;
    if (!mesh) return false;
    lo = mesh->bounds_min;
    hi = mesh->bounds_max;
    return true;
}

Vec3 joint_position(u32 j) {
    const Mat4 m = g_mw.pose.joint_model(j);
    return {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
}

void bone_tree(const anim::SkinnedModel& model, i32 parent) {
    for (u32 j = 0; j < model.joint_count(); ++j) {
        if (model.joint_parent(j) != parent) continue;
        bool has_children = false;
        for (u32 k = 0; k < model.joint_count() && !has_children; ++k) has_children = model.joint_parent(k) == static_cast<i32>(j);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | (has_children ? 0 : ImGuiTreeNodeFlags_Leaf);
        if (static_cast<i32>(j) == g_mw.bone) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(j + 1)), flags, "%s", model.joint_name(j));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_mw.bone = static_cast<i32>(j);
        if (open) {
            bone_tree(model, static_cast<i32>(j));
            ImGui::TreePop();
        }
    }
}

} // namespace

void open_mesh_editor(EditorState& s, const asset::AssetRecord& record) {
    g_mw.open = true;
    g_mw.record = record;
    asset::to_string(record.guid, g_mw.key);
    std::snprintf(g_mw.name, sizeof g_mw.name, "%s", record.name);
    g_mw.meta = asset::AssetMeta{};
    s.registry->load_meta(record, g_mw.meta);
    g_mw.meta_dirty = false;
    g_mw.bone = -1;
    g_mw.zoom = 1.0f;
    g_mw.morph[0] = '\0';
    g_mw.morphed_for = 0;
    ImGui::SetWindowFocus("###MeshEditor");
}

void render_mesh_editor(rhi::Device& device, const f32 background[4]) {
    if (!g_mw.open || !g_mw.state) return;
    EditorState& s = *g_mw.state;
    g_mw.device = &device;
    if (!g_mw.target.valid()) g_mw.target = device.create_render_target(kSize, kSize);
    if (!g_mw.target.valid()) return;
    Vec3 lo, hi;
    const SkinnedEntry* skinned;
    const CachedMesh* mesh;
    if (!bounds(s, lo, hi, skinned, mesh)) return;
    const Vec3 centre = (lo + hi) * 0.5f;
    const f32 radius = std::fmax(length(hi - lo) * 0.5f, 0.01f);
    const f32 distance = radius / std::sin(0.3f) * 1.05f * g_mw.zoom;
    const Vec3 dir{std::cos(g_mw.pitch) * std::cos(g_mw.yaw), std::cos(g_mw.pitch) * std::sin(g_mw.yaw), std::sin(g_mw.pitch)};
    const Vec3 eye = centre + dir * distance;
    g_mw.view_proj = perspective(0.6f, 1.0f, distance * 0.01f, distance + radius * 4.0f) * look_at(eye, centre, Vec3{0, 0, 1});
    const f32 dark[4] = {background[0] * 0.7f, background[1] * 0.7f, background[2] * 0.7f, 1.0f};
    device.set_render_target(g_mw.target, dark);
    rhi::MeshDraw draw{};
    std::memcpy(draw.mvp, g_mw.view_proj.m, sizeof draw.mvp);
    const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(draw.normal_matrix, identity, sizeof identity);
    std::memcpy(draw.world, identity, sizeof identity);
    draw.camera_position[0] = eye.x;
    draw.camera_position[1] = eye.y;
    draw.camera_position[2] = eye.z;
    const f32 grey[4] = {0.75f, 0.75f, 0.78f, 1.0f};
    std::memcpy(draw.color, grey, sizeof grey);
    if (skinned) {
        g_mw.pose.sample(*skinned->model, nullptr, 0.0f, false);  // the rest pose
        draw.vertices = skinned->vertices;
        if (g_mw.morph[0]) {  // the previewed morph target mixed into the rest pose
            u32 bits;
            std::memcpy(&bits, &g_mw.morph_weight, sizeof bits);
            const u64 want = (static_cast<u64>(skinned->vertices.value) << 32 | bits);  // picking another morph clears morphed_for
            if (want != g_mw.morphed_for || !g_mw.morphed.valid()) {
                if (g_mw.morphed.valid()) device.destroy_buffer(g_mw.morphed);
                std::vector<rhi::SkinnedVertex> v(skinned->model->vertices.size());
                anim::apply_morphs(*skinned->model, [](const char* name) { return std::strcmp(name, g_mw.morph) == 0 ? g_mw.morph_weight : 0.0f; }, v.data());
                g_mw.morphed = device.create_buffer(rhi::BufferUsage::Vertex, v.data(), v.size() * sizeof(rhi::SkinnedVertex));
                g_mw.morphed_for = want;
            }
            if (g_mw.morphed.valid()) draw.vertices = g_mw.morphed;
        }
        draw.indices = skinned->indices;
        draw.index_count = skinned->index_count;
        draw.joint_rows = g_mw.pose.joint_rows();
        draw.joint_count = g_mw.pose.joint_count();
    } else {
        draw.vertices = mesh->gpu.vertices;
        draw.indices = mesh->gpu.indices;
        draw.index_count = mesh->gpu.index_count;
    }
    device.draw_mesh(draw);
    device.set_render_target({}, nullptr);
}

void draw_mesh_editor(EditorState& s) {
    g_mw.state = &s;
    if (!g_mw.open) return;
    char title[160];
    std::snprintf(title, sizeof title, "%s%s - Mesh Editor###MeshEditor", g_mw.name, g_mw.meta_dirty ? "*" : "");
    ImGui::SetNextWindowSize(ImVec2(980, 600), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return;
    }
    if (!open) {
        g_mw.open = false;
        ImGui::End();
        return;
    }
    Vec3 lo, hi;
    const SkinnedEntry* skinned;
    const CachedMesh* mesh;
    const bool loaded = bounds(s, lo, hi, skinned, mesh);
    // Preview (left): drag to orbit, wheel to zoom; bones and sockets drawn over it.
    ImGui::BeginChild("preview", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0), ImGuiChildFlags_Borders);
    const f32 side = std::fmin(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 4.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (g_mw.target.valid() && s.device) {
        const bool flipped = s.device->render_targets_flipped();
        ImGui::Image(static_cast<ImTextureID>(s.device->debug_ui_texture(g_mw.target)), ImVec2(side, side), ImVec2(0, flipped ? 1.0f : 0.0f), ImVec2(1, flipped ? 0.0f : 1.0f));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                g_mw.yaw -= d.x * 0.01f;
                g_mw.pitch = std::fmin(std::fmax(g_mw.pitch + d.y * 0.01f, -1.5f), 1.5f);
            }
            g_mw.zoom = std::fmin(std::fmax(g_mw.zoom * (1.0f - ImGui::GetIO().MouseWheel * 0.1f), 0.2f), 5.0f);
        }
        if (skinned) {  // bones (lines to their parents) and sockets over the picture
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto project = [&](Vec3 p, ImVec2& out) {
                const Mat4& m = g_mw.view_proj;
                const f32 x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
                const f32 y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
                const f32 w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
                if (w < 0.001f) return false;
                out = ImVec2(at.x + (x / w * 0.5f + 0.5f) * side, at.y + (0.5f - y / w * 0.5f) * side);
                return true;
            };
            const anim::SkinnedModel& model = *skinned->model;
            for (u32 j = 0; j < model.joint_count() && j < g_mw.pose.joint_count(); ++j) {
                ImVec2 a, b;
                const ImU32 color = static_cast<i32>(j) == g_mw.bone ? IM_COL32(243, 195, 18, 255) : IM_COL32(155, 225, 31, 200);
                if (!project(joint_position(j), a)) continue;
                dl->AddCircleFilled(a, static_cast<i32>(j) == g_mw.bone ? 4.0f : 2.5f, color);
                const i32 parent = model.joint_parent(j);
                if (parent >= 0 && project(joint_position(static_cast<u32>(parent)), b)) dl->AddLine(a, b, color, 1.5f);
            }
            for (const asset::MeshSocket& sock : g_mw.meta.sockets) {
                const i32 j = model.find_joint(sock.bone);
                ImVec2 p;
                if (j < 0) continue;
                const Mat4 m = g_mw.pose.joint_model(static_cast<u32>(j)) * trs(sock.position, sock.rotation, Vec3{1, 1, 1});
                if (!project(Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)}, p)) continue;
                dl->AddRectFilled(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4), IM_COL32(238, 167, 14, 255));
                dl->AddText(ImVec2(p.x + 6, p.y - 7), IM_COL32(238, 167, 14, 255), sock.name);
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Numbers, settings, skeleton (right).
    ImGui::BeginChild("side", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (!loaded) {
        ImGui::TextDisabled("This mesh did not load (see the Output Log).");
    } else {
        const Vec3 size = hi - lo;
        ImGui::Text("Size: %.2f x %.2f x %.2f m", static_cast<f64>(size.x), static_cast<f64>(size.y), static_cast<f64>(size.z));
        if (skinned) {
            ImGui::Text("Triangles: %u   Bones: %u", skinned->index_count / 3, skinned->model->joint_count());
        } else {
            ImGui::Text("Triangles: %u", mesh->gpu.index_count / 3);
            for (u32 l = 1; l < mesh->lod_count; ++l) ImGui::Text("LOD %u: %u triangles", l, mesh->lod_index_count[l] / 3);
            if (mesh->lod_count == 1) ImGui::TextDisabled("No extra LODs (under 512 triangles)");
        }
    }
    ImGui::SeparatorText("Import settings");
    DetailsStyle style;
    style.categories = "Asset,Import";
    if (draw_details(reflect::type_of<asset::AssetMeta>(), &g_mw.meta, &style)) g_mw.meta_dirty = true;
    if (ImGui::Button("Save")) {
        g_mw.meta_dirty = !s.registry->save_meta(g_mw.record, g_mw.meta);
        std::snprintf(s.status, sizeof s.status, g_mw.meta_dirty ? "Could not save %s.meta" : "Saved %s.meta (it reimports)", g_mw.name);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reimport")) {
        char source[300];
        s.registry->source_path(g_mw.record, source, sizeof source);
        std::error_code error;
        std::filesystem::last_write_time(source, std::filesystem::file_time_type::clock::now(), error);
        std::snprintf(s.status, sizeof s.status, "Reimporting %s", g_mw.name);
    }
    if (skinned) {
        ImGui::SeparatorText("Skeleton");
        ImGui::BeginChild("bones", ImVec2(0, 200), ImGuiChildFlags_Borders);
        bone_tree(*skinned->model, -1);
        ImGui::EndChild();
        if (!skinned->model->morphs.empty()) {
            ImGui::SeparatorText("Morph Targets");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##morph_weight", &g_mw.morph_weight, 0.0f, 1.0f, "Preview weight %.2f")) g_mw.morphed_for = 0;
            ImGui::BeginChild("morphs", ImVec2(0, 140), ImGuiChildFlags_Borders);
            const auto& morphs = skinned->model->morphs;
            for (usize i = 0; i < morphs.size(); ++i) {
                bool shown_before = false;  // one row per name (several meshes may share it)
                for (usize k = 0; k < i && !shown_before; ++k) shown_before = std::strcmp(morphs[k].name, morphs[i].name) == 0;
                if (shown_before) continue;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(morphs[i].name, std::strcmp(g_mw.morph, morphs[i].name) == 0)) {
                    const bool same = std::strcmp(g_mw.morph, morphs[i].name) == 0;
                    std::snprintf(g_mw.morph, sizeof g_mw.morph, "%s", same ? "" : morphs[i].name);  // click again: back to rest
                    g_mw.morphed_for = 0;
                    ImGui::SetClipboardText(morphs[i].name);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::TextDisabled("Click: preview it (name copied for SkeletalMesh > Morphs or lsd.set_morph)");
        }
        ImGui::SeparatorText("Sockets");
        ImGui::BeginDisabled(g_mw.bone < 0);
        if (ImGui::Button("+ Socket on the selected bone")) {
            asset::MeshSocket sock;
            std::snprintf(sock.name, sizeof sock.name, "Socket%u", g_mw.meta.sockets.count + 1);
            std::snprintf(sock.bone, sizeof sock.bone, "%s", skinned->model->joint_name(static_cast<u32>(g_mw.bone)));
            if (g_mw.meta.sockets.push_back(sock)) g_mw.meta_dirty = true;
        }
        ImGui::EndDisabled();
        i32 remove = -1;
        for (u32 i = 0; i < g_mw.meta.sockets.count; ++i) {
            asset::MeshSocket& sock = g_mw.meta.sockets[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(120);
            g_mw.meta_dirty |= ImGui::InputText("##name", sock.name, sizeof sock.name);
            ImGui::SameLine();
            ImGui::TextDisabled("on %s", sock.bone);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove = static_cast<i32>(i);
            g_mw.meta_dirty |= ImGui::DragFloat3("Offset", &sock.position.x, 0.005f);
            g_mw.meta_dirty |= ImGui::DragFloat3("Turn", &sock.rotation.x, 0.5f);
            ImGui::PopID();
        }
        if (remove >= 0) {
            g_mw.meta.sockets.remove(static_cast<u32>(remove));
            g_mw.meta_dirty = true;
        }
        if (g_mw.meta.sockets.count) ImGui::TextDisabled("Save to keep them. Attach To Socket puts an object there.");
    }
    ImGui::EndChild();
    ImGui::End();
}

void shutdown_mesh_editor() {
    if (g_mw.device && g_mw.target.valid()) g_mw.device->destroy_texture(g_mw.target);
    if (g_mw.device && g_mw.morphed.valid()) g_mw.device->destroy_buffer(g_mw.morphed);
    g_mw.target = {};
    g_mw.open = false;
}

} // namespace lsd::editor
