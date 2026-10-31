#pragma once
// Per-user editor settings, saved to Saved/editor_settings.json (like Unreal's EditorPerProjectUserSettings).
#include "lsd/reflect/reflect.h"

namespace lsd::editor {

// A folder starred in the Content Browser.
LSD_STRUCT()
struct FavouriteFolder {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char path[256] = "";
};

// One asset in a collection (Unreal's Collections): a named set of assets from any folders.
LSD_STRUCT()
struct CollectionEntry {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char collection[48] = "";
    LSD_PROPERTY(Edit) char guid[33] = "";
};

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
    LSD_PROPERTY(Edit, Category = "Content Browser", Min = 48, Max = 256) f32 tile_size = 96.0f;
    LSD_PROPERTY(Edit, Category = "Content Browser") bool list_view = false;
    LSD_PROPERTY(Edit, Category = "Content Browser", Widget = "Choice:Name,Type,Date Modified,Size") u32 sort_by = 0;
    LSD_PROPERTY(Edit, Category = "Content Browser") List<FavouriteFolder, 32> favourites;
    LSD_PROPERTY(Edit, Category = "Content Browser") List<CollectionEntry, 512> collections;
};

// A key the player rebound: in every mapping of `action` (an Input Action by asset name; Move, Look and
// Jump for the built-in controls), `from` (the key the mapping context has) is replaced by `to`.
LSD_STRUCT()
struct KeyBinding {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char action[64] = "";
    LSD_PROPERTY(Edit) char from[32] = "";
    LSD_PROPERTY(Edit) char to[32] = "";
};

// The player's own settings (Unreal's Game User Settings), saved to Saved/GameUserSettings.json and
// applied at once when changed: the game's options menu edits them from Lua (lsd.get_settings /
// lsd.set_settings). The first run picks the quality the graphics card suits.
LSD_STRUCT()
struct GameUserSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Graphics") bool fullscreen = false;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Low,Medium,High,Epic", Tooltip = "Shadows: Low none, Medium sun only and nearer, High as set, Epic further")
    u32 quality = 2;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1) f32 master_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1, Tooltip = "The Sound Class named Music") f32 music_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1, Tooltip = "The Sound Class named SFX") f32 effects_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Controls", Min = 0.1, Max = 5, Tooltip = "Times the Character's look sensitivity") f32 mouse_sensitivity = 1.0f;
    LSD_PROPERTY(Edit, Category = "Controls") bool invert_look = false;
    LSD_PROPERTY(Edit, Category = "Controls", Tooltip = "Keys the player rebound (lsd.rebind)") List<KeyBinding, 32> key_bindings;
    LSD_PROPERTY(Edit, Category = "Language", Tooltip = "A column of the project's string table (en, fr...); empty: the project's default") char language[16] = "";
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
    LSD_PROPERTY(Edit, Category = "Localization", Widget = "Asset:Table", Tooltip = "String table: a Data Table with Key, then one column per language (en, fr...)") char string_table[33] = "";
    LSD_PROPERTY(Edit, Category = "Localization", Tooltip = "The language the game starts in (a column of the string table)") char default_language[16] = "en";
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Validate Assets also checks name prefixes (SM_, T_, M_, MI_, BT_, UI_...)") bool check_naming = false;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Textures larger than this on a side are reported") u32 max_texture_size = 4096;
};

} // namespace lsd::editor
