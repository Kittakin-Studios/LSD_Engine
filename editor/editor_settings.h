#pragma once
// Per-user editor settings, saved to Saved/editor_settings.json (like Unreal's EditorPerProjectUserSettings).
#include "lsd/reflect/reflect.h"

namespace lsd::editor {

LSD_STRUCT()
struct EditorSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Startup", Tooltip = "Opened when the editor starts") char last_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Startup", Units = "min", Min = 0, Tooltip = "Autosave unsaved levels to Saved/Autosaves (0 = off)") f32 autosave_minutes = 5.0f;
    LSD_PROPERTY(Edit, Category = "Viewport", Units = "m/s", Min = 0.1, Max = 100) f32 camera_speed = 6.0f;
    LSD_PROPERTY(Edit, Category = "Snapping") bool snap = true;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "m", Min = 0.01) f32 snap_move = 0.5f;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "deg", Min = 1) f32 snap_angle = 15.0f;
    LSD_PROPERTY(Edit, Category = "Snapping", Min = 0.01) f32 snap_scale = 0.25f;
};

// Project-wide settings shared by the team, saved to Config/project.json (Unreal's Project Settings).
LSD_STRUCT()
struct ProjectSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Description") char project_name[64] = "LSD Project";
    LSD_PROPERTY(Edit, Category = "Description") char company[64] = "Kittakin Studios";
    LSD_PROPERTY(Edit, Category = "Maps", Tooltip = "Level the packaged game opens first (Content/Levels/...)") char game_default_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Maps", Tooltip = "Level the editor opens when there is no last level") char editor_startup_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Platform", Widget = "Asset:Texture", Tooltip = "The game window's icon (a square PNG, 256 x 256 is plenty)") char icon[33] = "";
    LSD_PROPERTY(Edit, Category = "Platform", Tooltip = "Steam: written to steam_appid.txt next to the packaged exe (0 = not on Steam)") u32 steam_app_id = 0;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Validate Assets also checks name prefixes (SM_, T_, M_, MI_, BT_, UI_...)") bool check_naming = false;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Textures larger than this on a side are reported") u32 max_texture_size = 4096;
};

} // namespace lsd::editor
