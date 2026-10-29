#include "editor_ui.h"

#include "lsd/debug_ui/details_panel.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API

#include <cmath>
#include <cstdio>

namespace lsd::editor {

namespace {

constexpr char kThemePath[] = "Config/editor_theme.json";
constexpr char kOutlinerWindow[] = "Outliner";
constexpr char kDetailsWindow[] = "Details";
constexpr char kContentWindow[] = "Content Browser";

ImU32 u32_color(const LinearColor& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec4 vec4(const LinearColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

// ---- Math for the viewport grid (Z up, like Unreal) ----

struct Vec3 { f32 x, y, z; };
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 normalize(Vec3 a) { const f32 l = std::sqrt(dot(a, a)); return a * (1.0f / l); }

struct View {
    Vec3 eye, right, up, forward;
    ImVec2 center;
    f32 focal;  // pixels per unit at depth 1
};

View make_view(const OrbitCamera& cam, ImVec2 pos, ImVec2 size) {
    View v;
    const f32 cp = std::cos(cam.pitch);
    v.eye = Vec3{cp * std::cos(cam.yaw), cp * std::sin(cam.yaw), std::sin(cam.pitch)} * cam.distance;
    v.forward = normalize(Vec3{0, 0, 0} - v.eye);
    v.right = normalize(cross(v.forward, Vec3{0, 0, 1}));
    v.up = cross(v.right, v.forward);
    v.center = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    v.focal = size.y * 0.5f / std::tan(cam.fov_y * 0.5f);
    return v;
}

// Draws a world-space line, clipped against the near plane so lines behind the camera vanish cleanly.
void draw_line(ImDrawList* dl, const View& v, Vec3 a, Vec3 b, ImU32 color, f32 thickness) {
    constexpr f32 kNear = 0.05f;
    Vec3 va{dot(a - v.eye, v.right), dot(a - v.eye, v.up), dot(a - v.eye, v.forward)};
    Vec3 vb{dot(b - v.eye, v.right), dot(b - v.eye, v.up), dot(b - v.eye, v.forward)};
    if (va.z < kNear && vb.z < kNear) return;
    if (va.z < kNear) va = va + (vb - va) * ((kNear - va.z) / (vb.z - va.z));
    if (vb.z < kNear) vb = vb + (va - vb) * ((kNear - vb.z) / (va.z - vb.z));
    const ImVec2 pa(v.center.x + va.x / va.z * v.focal, v.center.y - va.y / va.z * v.focal);
    const ImVec2 pb(v.center.x + vb.x / vb.z * v.focal, v.center.y - vb.y / vb.z * v.focal);
    dl->AddLine(pa, pb, color, thickness);
}

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

void draw_viewport(EditorState& s, ImVec2 pos, ImVec2 size) {
    if (size.x < 8 || size.y < 8) return;
    const EditorTheme& t = s.theme;
    ImGuiIO& io = ImGui::GetIO();

    // Right-drag orbits, wheel zooms (only when the mouse is over the viewport, not a panel).
    const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y)) && !io.WantCaptureMouse;
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

    // shortcut: grid drawn as ImGui lines on the CPU; becomes a GPU grid pass with the renderer (milestone 3).
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    const View v = make_view(s.camera, pos, size);
    if (s.show_grid) {
        constexpr int kHalf = 50;  // grid covers -50..50 units
        for (int i = -kHalf; i <= kHalf; ++i) {
            if (i == 0) continue;  // axis lines drawn on top
            const bool major = i % 5 == 0;
            const ImU32 color = u32_color(major ? t.grid_major : t.grid_minor);
            const f32 thickness = major ? 1.5f : 1.0f;
            const f32 f = static_cast<f32>(i);
            draw_line(dl, v, Vec3{f, -kHalf, 0}, Vec3{f, kHalf, 0}, color, thickness);
            draw_line(dl, v, Vec3{-kHalf, f, 0}, Vec3{kHalf, f, 0}, color, thickness);
        }
        draw_line(dl, v, Vec3{-kHalf, 0, 0}, Vec3{kHalf, 0, 0}, u32_color(t.axis_x), 2.0f);
        draw_line(dl, v, Vec3{0, -kHalf, 0}, Vec3{0, kHalf, 0}, u32_color(t.axis_y), 2.0f);
    }

    // Axis gizmo, bottom-left: world axes as seen from the camera.
    if (s.show_axis_gizmo) {
        const ImVec2 origin(pos.x + 48, pos.y + size.y - 48);
        const struct { Vec3 dir; const LinearColor* color; const char* label; } axes[3] = {
            {{1, 0, 0}, &t.axis_x, "X"}, {{0, 1, 0}, &t.axis_y, "Y"}, {{0, 0, 1}, &t.axis_z, "Z"}};
        for (const auto& a : axes) {
            const ImVec2 tip(origin.x + dot(a.dir, v.right) * 32.0f, origin.y - dot(a.dir, v.up) * 32.0f);
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
        ImGui::TextDisabled("RMB drag: orbit  |  wheel: zoom");
    }
    ImGui::End();
}

// ---- Outliner ----

enum class RowKind { Mesh, Component, Blueprint, Folder, Vfx };
struct OutlineRow { const char* name; RowKind kind; int depth; };

// shortcut: sample rows from the layout mockup; replaced by the World's entities in milestone 3.
constexpr OutlineRow kSampleRows[] = {
    {"SM_Cube", RowKind::Mesh, 0},
    {"Actor Component", RowKind::Component, 0},
    {"BP_ThirdPersonCharacter", RowKind::Blueprint, 0},
    {"Folder_01", RowKind::Folder, 0},
    {"VFX_Fire_01", RowKind::Vfx, 1},
};

void draw_outliner(EditorState& s) {
    const EditorTheme& t = s.theme;
    static int active_tab = 0;
    if (!begin_panel(kOutlinerWindow)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("OutlinerTabs")) {
        if (themed_tab("Outline", 0, active_tab, t.outline_tab, t.panel_background, LinearColor::hex(0xFFFFFF))) {
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, vec4(t.outline_scrollbar));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.outline_row_b));
            ImGui::BeginChild("OutlineRows", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_None);
            const f32 row_height = ImGui::GetTextLineHeight() + 10.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const f32 left = ImGui::GetWindowPos().x;
            const f32 right = left + ImGui::GetWindowWidth();
            int rows = static_cast<int>(ImGui::GetWindowHeight() / row_height) + 1;
            constexpr int kSampleCount = static_cast<int>(sizeof kSampleRows / sizeof kSampleRows[0]);
            rows = rows > kSampleCount ? rows : kSampleCount;
            for (int i = 0; i < rows; ++i) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const OutlineRow* row = i < kSampleCount ? &kSampleRows[i] : nullptr;
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
                char label[96];
                std::snprintf(label, sizeof label, "%s%s", row->depth ? "\xE2\x86\xB3 " : "", row->name);
                ImGui::PushStyleColor(ImGuiCol_Text, vec4(*text));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<f32>(row->depth) * 22.0f);
                if (ImGui::Selectable(label, s.selected_row == i, ImGuiSelectableFlags_SpanAvailWidth,
                                      ImVec2(0, row_height - ImGui::GetStyle().ItemSpacing.y))) {
                    s.selected_row = i;
                }
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::TextDisabled("Sample rows: the level's objects appear in milestone 3");
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
        if (themed_tab("Details", 0, s.details_tab, t.details_tab_active, t.details_tab, t.text_on_bright)) {
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
            const bool root = ImGui::TreeNodeEx("Content", node);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, vec4(t.folder_tree_text));
            if (root) {
                // shortcut: sample folders from the mockup; the asset registry scans Content/ in milestone 2.
                if (ImGui::TreeNodeEx("Character", node)) {
                    if (ImGui::TreeNodeEx("Models", node)) {
                        ImGui::TreeNodeEx("3D", node | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                        ImGui::TreeNodeEx("2D", node | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                        ImGui::TreePop();
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(t.content_area));
            ImGui::BeginChild("Assets", ImVec2(0, 0), ImGuiChildFlags_None);
            ImGui::TextDisabled("Imported assets appear here once importers land (milestone 2).");
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

    // The central node is left empty and transparent: the 3D viewport shows through it.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) draw_viewport(s, central->Pos, central->Size);
}

void load_theme(EditorTheme& theme) { reflect::load_file(theme, kThemePath); }

} // namespace lsd::editor
