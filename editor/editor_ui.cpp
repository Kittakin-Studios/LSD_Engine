#include "editor_ui.h"

#include "thumbnails.h"

#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace lsd::editor {

namespace {

constexpr char kThemePath[] = "Config/editor_theme.json";
constexpr char kContentDir[] = "Content";
constexpr char kAssetPayload[] = "LSD_ASSET";  // drag-and-drop payload: an asset::Guid
constexpr char kOutlinerWindow[] = "Outliner";
constexpr char kDetailsWindow[] = "Details";
constexpr char kContentWindow[] = "Content Browser";

ImU32 u32_color(const LinearColor& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec4 vec4(const LinearColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// ---- Layout ----

void build_default_layout(ImGuiID dockspace, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID right = 0, left = 0, right_top = 0, right_bottom = 0, content = 0, viewport = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.5f, &right_top, &right_bottom);
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

// ---- Viewport ----

// Where the mouse ray hits the floor (z = 0); the origin if the ray points at the sky.
Vec3 floor_point(const OrbitCamera& cam, ImVec2 pos, ImVec2 size, ImVec2 mouse) {
    const Mat4 view = camera_view(cam);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    const Vec3 back{view.at(2, 0), view.at(2, 1), view.at(2, 2)};
    const f32 tan_half = std::tan(cam.fov_y * 0.5f);
    const f32 nx = ((mouse.x - pos.x) / size.x * 2.0f - 1.0f) * tan_half * (size.x / size.y);
    const f32 ny = (1.0f - (mouse.y - pos.y) / size.y * 2.0f) * tan_half;
    const Vec3 dir = normalize(right * nx + up * ny - back);
    const Vec3 eye = camera_eye(cam);
    if (dir.z > -1e-4f) return {0, 0, 0};
    return eye + dir * (-eye.z / dir.z);
}

void draw_viewport(EditorState& s, ImVec2 pos, ImVec2 size) {
    if (size.x < 8 || size.y < 8) return;
    const EditorTheme& t = s.theme;
    ImGuiIO& io = ImGui::GetIO();

    // Invisible window covering the viewport: owns its mouse input and accepts asset drops.
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##ViewportInput", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) s.selected_row = -1;  // click on empty space deselects (no picking yet)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            std::memcpy(&s.place_asset, payload->Data, sizeof s.place_asset);
            s.place_position = floor_point(s.camera, pos, size, io.MousePos);
            s.place_requested = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();

    // Right-drag orbits, wheel zooms.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) s.orbiting = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) s.orbiting = false;
    if (s.orbiting) {
        s.camera.yaw -= io.MouseDelta.x * 0.006f;
        s.camera.pitch += io.MouseDelta.y * 0.006f;
        s.camera.pitch = s.camera.pitch < 0.05f ? 0.05f : s.camera.pitch > 1.5f ? 1.5f : s.camera.pitch;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        s.camera.distance *= std::pow(0.88f, io.MouseWheel);
        s.camera.distance = s.camera.distance < 1.0f ? 1.0f : s.camera.distance > 200.0f ? 200.0f : s.camera.distance;
    }

    s.viewport_rect[0] = pos.x;
    s.viewport_rect[1] = pos.y;
    s.viewport_rect[2] = size.x;
    s.viewport_rect[3] = size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    // Camera axes from the view matrix rows: row 0 = right, row 1 = up.
    const Mat4 view = camera_view(s.camera);
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};

    // Axis gizmo, bottom-left: world axes as seen from the camera.
    if (s.show_axis_gizmo) {
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
        ImGui::SetItemTooltip("Orthographic views arrive with the editor viewport (milestone 3)");
        ImGui::SameLine();
        ImGui::Button("Lit");
        ImGui::SetItemTooltip("View modes (Unlit, Wireframe...) arrive with the renderer");
        ImGui::SameLine();
        if (ImGui::Button("Show")) ImGui::OpenPopup("ShowMenu");
        if (ImGui::BeginPopup("ShowMenu")) {
            ImGui::Checkbox("Grid", &s.show_grid);
            ImGui::Checkbox("Axis gizmo", &s.show_axis_gizmo);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (s.show_grid) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool grid_was_on = s.show_grid;
        if (ImGui::Button("Grid")) s.show_grid = !s.show_grid;
        if (grid_was_on) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("RMB drag: orbit  |  wheel: zoom  |  drag assets in from the Content Browser");
    }
    ImGui::End();
}

// ---- Outliner ----

void draw_outliner(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 0;
    if (!begin_panel(kOutlinerWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("OutlinerTabs")) {
        if (themed_tab("Outline", 0, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, vec4(t.outline_scrollbar));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.outline_row_b));
            ImGui::BeginChild("OutlineRows", ImVec2(0, 0), ImGuiChildFlags_None);
            const f32 row_height = ImGui::GetTextLineHeight() + 10.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const f32 left = ImGui::GetWindowPos().x;
            const f32 right = left + ImGui::GetWindowWidth();
            int rows = static_cast<int>(ImGui::GetWindowHeight() / row_height) + 1;
            rows = rows > s.row_count ? rows : s.row_count;
            for (int i = 0; i < rows; ++i) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const OutlineRow* row = i < s.row_count ? &s.rows[i] : nullptr;
                dl->AddRectFilled(ImVec2(left, p.y), ImVec2(right, p.y + row_height), u32_color((i & 1) ? t.outline_row_b : t.outline_row_a));
                if (row && row->kind == RowKind::Folder) dl->AddRectFilled(ImVec2(left, p.y), ImVec2(right, p.y + row_height), u32_color(t.folder_row));
                if (row && row->kind == RowKind::Vfx) {
                    const f32 indent = p.x + static_cast<f32>(row->depth) * 22.0f;
                    dl->AddRectFilled(ImVec2(indent, p.y), ImVec2(right, p.y + row_height), u32_color(t.vfx_row));
                }
                if (!row) { ImGui::Dummy(ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y)); continue; }

                const LinearColor* text = &t.text_mesh;
                switch (row->kind) {
                    case RowKind::Mesh:      text = &t.text_mesh; break;
                    case RowKind::Component: text = &t.text_component; break;
                    case RowKind::Blueprint: text = &t.text_blueprint; break;
                    case RowKind::Folder:    text = &t.text_folder; break;
                    case RowKind::Vfx:       text = &t.text_vfx; break;
                }
                if (row->depth) {  // child hook drawn as lines: the UI font has no arrow glyph
                    const f32 hx = p.x + static_cast<f32>(row->depth) * 22.0f - 14.0f;
                    const f32 mid = p.y + row_height * 0.5f;
                    dl->AddLine(ImVec2(hx, p.y + 4.0f), ImVec2(hx, mid), u32_color(*text), 1.5f);
                    dl->AddLine(ImVec2(hx, mid), ImVec2(hx + 9.0f, mid), u32_color(*text), 1.5f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, vec4(*text));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<f32>(row->depth) * 22.0f);
                if (ImGui::Selectable(row->name, s.selected_row == i, ImGuiSelectableFlags_SpanAvailWidth,
                                      ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y))) {
                    s.selected_row = i;
                    s.selected_asset = {};
                }
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::EndTabItem();
        }
        if (themed_tab("Hierarchy", 1, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            ImGui::TextDisabled("The parent/child hierarchy arrives with the World (milestone 3).");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Details ----

void draw_details_panel(EditorState& s) {
    EditorTheme& t = s.theme;
    if (!begin_panel(kDetailsWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("DetailsTabs")) {
        const asset::AssetRecord* selected = s.registry && s.selected_asset.valid() ? s.registry->find(s.selected_asset) : nullptr;
        if (themed_tab("Details", 0, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
            if (selected) {
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
                ImGui::TextDisabled("Editor Theme  (%s)", kThemePath);
                if (ImGui::Button("Save")) {
                    std::snprintf(s.status, sizeof s.status, reflect::save_file(t, kThemePath) ? "Saved %s" : "Could not save %s", kThemePath);
                }
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    std::snprintf(s.status, sizeof s.status, reflect::load_file(t, kThemePath) ? "Loaded %s" : "Could not load %s", kThemePath);
                    apply_theme(t);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    t = EditorTheme{};
                    apply_theme(t);
                    std::snprintf(s.status, sizeof s.status, "Reset to defaults (not saved)");
                }
                if (s.status[0]) ImGui::TextDisabled("%s", s.status);
                ImGui::BeginChild("DetailsRows");
                const DetailsStyle style{{u32_color(t.details_row_a), u32_color(t.details_row_b)}};
                if (draw_details(reflect::type_of<EditorTheme>(), &t, &style)) apply_theme(t);
                ImGui::EndChild();
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
        if (open) {
            draw_folder_tree(s, entry.path(), flags);
            ImGui::TreePop();
        }
    }
    return any;
}

// Asset tiles for the current folder or search, each draggable into the viewport.
void draw_asset_grid(EditorState& s) {
    const EditorTheme& t = s.theme;
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##search", "Search assets", s.search, sizeof s.search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("##type", &s.type_filter, "All types\0Meshes\0");
    ImGui::SameLine();
    ImGui::TextDisabled(s.search[0] ? "Results in all folders" : "Content/%s", s.content_folder);
    ImGui::Separator();
    if (!s.registry) return;

    static usize hits[1024];
    const asset::AssetType type = s.type_filter == 1 ? asset::AssetType::Mesh : asset::AssetType::Unknown;
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
    if (count == 0 && folder_count == 0) {
        ImGui::TextDisabled(s.search[0] ? "No assets match." : "No assets here. Drag .fbx, .gltf or .glb files from Explorer onto the editor to import them into this folder.");
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
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            char next[256];
            std::snprintf(next, sizeof next, "%s%s%s", s.content_folder, s.content_folder[0] ? "/" : "", folders[f]);
            std::snprintf(s.content_folder, sizeof s.content_folder, "%s", next);
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
        ImGui::PushID(static_cast<int>(hits[n]));
        if (tile % static_cast<usize>(columns) != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = r.guid == s.selected_asset;
        if (ImGui::Selectable("##tile", selected, 0, ImVec2(kTile, kTile + ImGui::GetTextLineHeightWithSpacing()))) {
            s.selected_asset = r.guid;
            s.selected_row = -1;
            s.selected_meta = asset::AssetMeta{};
            s.registry->load_meta(r, s.selected_meta);
            s.status[0] = '\0';
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kAssetPayload, &r.guid, sizeof r.guid);
            ImGui::Text("Place %s", r.name);
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nContent/%s", r.name, r.path);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 lo(p.x + 6, p.y + 6), hi(p.x + kTile - 6, p.y + kTile - 6);
        const u64 thumbnail = s.thumbnails ? s.thumbnails->get(r.guid) : 0;
        if (thumbnail) {
            const bool flip = s.thumbnails->flipped();
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(thumbnail)), lo, hi, ImVec2(0, flip ? 1.0f : 0.0f), ImVec2(1, flip ? 0.0f : 1.0f));
        } else {  // still rendering (or failed): type badge
            dl->AddRectFilled(lo, hi, u32_color(t.outline_row_b), 4.0f);
            const char* badge = asset::to_string(r.type);
            const ImVec2 badge_size = ImGui::CalcTextSize(badge);
            dl->AddText(ImVec2(p.x + (kTile - badge_size.x) * 0.5f, p.y + (kTile - badge_size.y) * 0.5f), u32_color(t.text_mesh), badge);
        }
        dl->AddRect(lo, hi, u32_color(selected ? t.details_tab_active : t.text_mesh), 4.0f, 0, 2.0f);
        dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + kTile, p.y + kTile + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(p.x + 4, p.y + kTile), ImGui::GetColorU32(ImGuiCol_Text), r.name);
        dl->PopClipRect();
        ImGui::EndGroup();
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
        ImGui::EndTabBar();
    }
    ImGui::End();
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
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImHashStr("EditorDockspace");
    const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspace);
    if (!existing || !existing->IsSplitNode()) build_default_layout(dockspace, main->WorkSize);
    ImGui::DockSpaceOverViewport(dockspace, main, ImGuiDockNodeFlags_PassthruCentralNode);

    draw_outliner(s);
    draw_details_panel(s);
    draw_content_browser(s);
    if (s.selected_row >= 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) s.delete_row = s.selected_row;

    // The central node is left empty and transparent: the 3D viewport shows through it.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) draw_viewport(s, central->Pos, central->Size);
}

Vec3 camera_eye(const OrbitCamera& cam) {
    const f32 cp = std::cos(cam.pitch);
    return Vec3{cp * std::cos(cam.yaw), cp * std::sin(cam.yaw), std::sin(cam.pitch)} * cam.distance;
}

Mat4 camera_view(const OrbitCamera& cam) { return look_at(camera_eye(cam), Vec3{0, 0, 0}, Vec3{0, 0, 1}); }

Mat4 camera_projection(const OrbitCamera& cam, f32 aspect) { return perspective(cam.fov_y, aspect, 0.05f, 1000.0f); }

void load_theme(EditorTheme& theme) { reflect::load_file(theme, kThemePath); }

} // namespace lsd::editor
