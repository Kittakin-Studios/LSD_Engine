#pragma once
// Editor shell: dock layout (Viewport, Outliner, Details, Content Browser) drawn with Dear ImGui.
// The UI only edits EditorState; main.cpp acts on the requests it leaves there (place, delete).
#include "editor_theme.h"

#include "lsd/asset/asset_registry.h"

#include "lsd/foundation/math.h"

namespace lsd::editor {

struct OrbitCamera {
    f32 yaw = -2.4f;     // radians around +Z
    f32 pitch = 0.45f;   // radians above the floor
    f32 distance = 7.0f;
    f32 fov_y = 1.0f;    // radians
};

// One Outliner row. The kind picks the text/row colours from the theme.
enum class RowKind { Mesh, Component, Blueprint, Folder, Vfx };
struct OutlineRow {
    const char* name;
    RowKind kind;
    int depth;  // 0 = top level, 1 = inside the folder above
};

struct EditorState {
    EditorTheme theme;
    OrbitCamera camera;
    bool show_grid = true;
    bool show_axis_gizmo = true;
    bool orbiting = false;
    const OutlineRow* rows = nullptr;  // what is in the scene, set by main.cpp
    int row_count = 0;
    int selected_row = -1;
    int details_tab = 0;

    // Content Browser
    const asset::AssetRegistry* registry = nullptr;
    char content_folder[256] = "";  // selected folder relative to Content/ ("" = Content itself)
    char search[96] = "";
    int type_filter = 0;            // 0 = all types, 1 = meshes
    asset::Guid selected_asset;     // shown in Details when valid
    asset::AssetMeta selected_meta;

    // Requests for main.cpp, cleared once handled
    bool place_requested = false;
    asset::Guid place_asset;
    Vec3 place_position;
    int delete_row = -1;
    char status[160] = "";
    f32 viewport_rect[4] = {};  // x, y, width, height in window pixels, set by draw_editor
};

Vec3 camera_eye(const OrbitCamera& camera);
Mat4 camera_view(const OrbitCamera& camera);
Mat4 camera_projection(const OrbitCamera& camera, f32 aspect);

// Loads Config/editor_theme.json into theme if it exists.
void load_theme(EditorTheme& theme);

// Pushes theme colours and font size into the ImGui style. Call when the theme changes.
void apply_theme(const EditorTheme& theme);

// Draws the whole editor for this frame (between ImGui::NewFrame and ImGui::Render).
void draw_editor(EditorState& state);

} // namespace lsd::editor
