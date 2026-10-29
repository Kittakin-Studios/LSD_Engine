#pragma once
// Editor shell: dock layout (Viewport, Outliner, Details, Content Browser) drawn with Dear ImGui.
// Panels show sample data until the World (milestone 3) and asset registry (milestone 2) exist.
#include "editor_theme.h"

namespace lsd::editor {

struct OrbitCamera {
    f32 yaw = -2.4f;     // radians around +Z
    f32 pitch = 0.45f;   // radians above the floor
    f32 distance = 18.0f;
    f32 fov_y = 1.0f;    // radians
};

struct EditorState {
    EditorTheme theme;
    OrbitCamera camera;
    bool show_grid = true;
    bool show_axis_gizmo = true;
    bool orbiting = false;
    int selected_row = -1;
    int details_tab = 0;
    char status[160] = "";
};

// Loads Config/editor_theme.json into theme if it exists.
void load_theme(EditorTheme& theme);

// Pushes theme colours and font size into the ImGui style. Call when the theme changes.
void apply_theme(const EditorTheme& theme);

// Draws the whole editor for this frame (between ImGui::NewFrame and ImGui::Render).
void draw_editor(EditorState& state);

} // namespace lsd::editor
