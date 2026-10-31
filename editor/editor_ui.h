#pragma once
// Editor shell: menu bar, dock layout (Viewport, Outliner, Details, Content Browser / Place)
// drawn with Dear ImGui. The UI edits the World directly and names each finished edit, which
// becomes one undo step. File and Play requests are left in EditorState for main.cpp.
#include "editor_settings.h"
#include "editor_theme.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/math.h"
#include "lsd/world/world.h"

namespace lsd::rhi {
class Device;
}

namespace lsd::editor {

class MeshCache;
class ThumbnailCache;
class Transactions;

enum class ViewType : u8 { Perspective, Top, Front, Right };  // ortho views look like Blender's numpad views
enum class ViewMode : u8 { Lit, Unlit, Wireframe };

// Unreal-style viewport camera: a position and a look direction. In the orthographic views
// `position` is the centre of the view and `ortho_height` how many metres fit vertically.
struct EditorCamera {
    ViewType view = ViewType::Perspective;
    f32 ortho_height = 20.0f;
    Vec3 position{-4.65f, -4.26f, 3.04f};
    f32 yaw = 0.74f;     // radians around +Z; 0 looks along +X
    f32 pitch = -0.45f;  // radians; negative looks down
    f32 fov_y = 1.0f;
    f32 pivot_distance = 7.0f;  // Alt+LMB orbits around the point this far ahead
};

enum class GizmoMode { Translate, Rotate, Scale };

struct EditorState {
    EditorTheme theme;
    EditorSettings settings;
    EditorCamera camera;
    bool show_grid = true;
    bool show_axis_gizmo = true;
    i32 force_lod = -1;  // r.ForceLOD: every mesh at that LOD (-1: by distance)
    bool show_navigation = false;  // P: the navmesh in green (rebuilt by main.cpp about once a second)
    Vec3 nav_lines[32768];         // line pairs
    u32 nav_line_count = 0;
    bool game_view = false;  // G: hide grid, gizmos and icons
    ViewMode view_mode = ViewMode::Lit;
    EditorCamera perspective_camera;  // restored when leaving an orthographic view
    bool show_history = false;
    bool show_preferences = false;
    bool show_stats = false;  // `stat fps`: fps, frame time, draw calls, triangles
    bool show_audio_stats = false;  // `stat audio`: active voices
    bool view_mode_unlit_or_wire() const { return (!playing || ejected) && view_mode != ViewMode::Lit; }
    class SoundPlayer* sounds = nullptr;  // set by main.cpp
    u32 preview_voice = 0;  // Content Browser sound preview
    u32 stat_draws = 0;       // set by main.cpp after drawing
    u32 stat_triangles = 0;
    u32 stat_culled = 0;      // outside the view: not drawn
    f32 stat_game_ms = 0.0f, stat_render_ms = 0.0f, stat_ui_ms = 0.0f;  // stat unit (CPU)
    EditorCamera bookmarks[10];  // Ctrl+0..9 stores, 0..9 jumps (like Unreal)
    bool bookmark_set[10] = {};
    int details_tab = 0;
    char status[160] = "";

    // Level
    world::World* world = nullptr;
    MeshCache* meshes = nullptr;
    Transactions* history = nullptr;
    // Selection: `selected` is the primary (last clicked: Details and the gizmo pivot follow it);
    // `selection` holds every selected entity, primary included.
    world::Entity selected;
    i32 spline_point = -1;  // selected point of the selected object's Spline (the gizmo moves it); -1 = none
    static constexpr u32 kMaxSelection = 1024;
    world::Entity selection[kMaxSelection];
    u32 selection_count = 0;
    bool is_selected(world::Entity e) const {
        for (u32 i = 0; i < selection_count; ++i) {
            if (selection[i] == e) return true;
        }
        return false;
    }
    GizmoMode gizmo = GizmoMode::Translate;
    bool gizmo_local = false;
    char level_path[260] = "";  // "" = never saved
    char pending_edit[64] = "";  // label of an edit in progress; committed when the mouse and widgets are idle

    // Content Browser
    asset::AssetRegistry* registry = nullptr;
    ThumbnailCache* thumbnails = nullptr;
    class TextureCache* textures = nullptr;  // set by main.cpp (texture thumbnails)
    class SkinnedCache* skinned = nullptr;   // set by main.cpp (rigged meshes: bounds, drop)
    class ParticleEditor* particle_editor = nullptr;  // set by main.cpp
    rhi::Device* device = nullptr;
    char content_folder[256] = "";  // selected folder relative to Content/ ("" = Content itself)
    char search[96] = "";
    int type_filter = 0;            // 0 = all types, 1 = meshes, 2 = levels
    asset::Guid selected_asset;     // shown in Details when valid
    static constexpr u32 kMaxAssetSelection = 256;
    asset::Guid asset_selection[kMaxAssetSelection];  // Ctrl / Shift-click picks several (selected_asset is one of them)
    u32 asset_selection_count = 0;
    char active_collection[48] = "";  // the Content Browser shows this collection instead of a folder
    bool show_content_tab = false;    // Find in Content Browser: switch the bottom panel to Content
    asset::AssetMeta selected_meta;

    // Rename / delete dialogs (Content Browser)
    asset::Guid rename_asset;       // valid: renaming this asset
    char rename_folder[256] = "";   // non-empty: renaming this folder (relative to Content/)
    char rename_buffer[96] = "";
    bool open_rename = false;
    asset::Guid delete_asset;
    char delete_folder[256] = "";
    bool open_delete = false;

    // Requests for main.cpp, cleared once handled
    bool new_level_requested = false;
    char open_level_requested[260] = "";
    bool save_requested = false;
    char save_as_requested[260] = "";  // path
    bool play_requested = false;
    f32 smoke_test_seconds = 0.0f;  // > 0: play the level that long and report its errors (Tools > Smoke Test)
    bool run_unit_tests = false;    // Tools > Run Unit Tests
    u32 high_res_shot = 0;          // HighResShot <n>: the main view at n times its size, to Saved/Screenshots
    bool stop_requested = false;
    bool playing = false;
    bool quit_requested = false;   // window closed with unsaved changes: the UI asks first
    bool quit_confirmed = false;
    bool package_requested = false;  // File > Package Project
    bool build_lighting_requested = false;  // Build > Build Lighting
    bool import_requested = false;          // File > Import...
    f32 build_lighting_progress = -1.0f;    // 0..1 while baking, -1 otherwise
    bool game_mode = false;          // a packaged game (or -game): Play only, no editor
    bool quit_after_save = false;
    char recovery_path[260] = "";  // autosave left by a crash: the UI offers to restore it
    bool restore_requested = false;
    bool mouse_captured = false;  // Play: mouse drives the player's view
    bool paused = false;          // Play: Pause key; frame step with ] while paused
    bool step_requested = false;
    bool ejected = false;         // Play: F8 frees the editor camera (select and inspect) while the game runs
    Vec3 player_feet;             // Play: where the player is, for the ejected view
    bool show_world_settings = false;
    bool show_message_log = false;
    bool show_remote_console = false;  // Tools > Remote Console
    bool show_memory = false;          // Window > Memory (memreport)
    bool show_debugger = false;        // Tools > Lua Debugger
    static constexpr u32 kMaxKept = 64;
    world::Entity kept[kMaxKept];             // Keep Simulation Changes: copied back into the level when Play stops
    u32 kept_count = 0;
    bool show_project_settings = false;
    ProjectSettings project;
    // Viewports: one, or a 2x2 layout (perspective, top, front, right). `camera` always belongs to the
    // active viewport (the last one clicked); `views` keeps the others' cameras.
    bool quad_view = false;
    EditorCamera views[4];
    int active_view = 0;
    u32 viewport_count = 0;          // set by draw_editor
    f32 viewport_rects[4][4] = {};   // x, y, width, height in window pixels
    Mat4 drawn_view_proj[4];         // each viewport's camera as last drawn (the player's in Play): debug drawing
    const EditorCamera& viewport_camera(u32 i) const { return static_cast<int>(i) == active_view ? camera : views[i]; }
};

// Perspective <-> orthographic; restores the perspective camera when coming back.
void set_view(EditorState& state, ViewType view);
// Creates IA_Move, IA_Look, IA_Jump and IMC_Default in the open Content Browser folder.
void create_input_setup(EditorState& state);
// Reloads every prefab instance in the level from its asset (after opening a level).
u32 refresh_prefabs(EditorState& state);
// Selects the first asset whose name matches (for scripted screenshots and the console).
bool select_asset(EditorState& state, const char* name);
// The Details panel's working copy of a data asset (unsaved edits included) if it is showing that
// asset as `type`, else nullptr. Editor windows preview from it (the Particle Editor).
void* edited_data_asset(const asset::Guid& guid, const reflect::TypeInfo& type);
bool asset_matches(const asset::AssetRecord& record, const char* type);  // "Mesh", "Material", a data type name...

// One viewport, or the 2x2 layout (perspective, top, front, right).
void set_layout(EditorState& state, bool quad);

Vec3 camera_forward(const EditorCamera& camera);
Mat4 camera_view(const EditorCamera& camera);
Mat4 camera_projection(const EditorCamera& camera, f32 aspect);

// Starts capturing log lines for the Output Log tab. Call once before anything logs that should show.
void install_output_log();
void clear_output_log();

// Loads Config/editor_theme.json into theme if it exists.
void load_theme(EditorTheme& theme);

// Pushes theme colours and font size into the ImGui style. Call when the theme changes.
void apply_theme(const EditorTheme& theme);

// Draws the whole editor for this frame (between ImGui::NewFrame and ImGui::Render).
void draw_editor(EditorState& state);
// Game mode's UI: none, the viewport fills the window; Esc quits, a click takes the mouse back.
void draw_game(EditorState& state);

// Fills a new level with a floor and a Player Start, like Unreal's default new level.
void make_default_level(world::World& world);

} // namespace lsd::editor
