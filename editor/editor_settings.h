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

// A level opened or saved lately (File > Recent Levels), newest first.
LSD_STRUCT()
struct RecentLevel {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char path[260] = "";
};

// A workspace tab the user added with + (a copy of another workspace's arrangement).
LSD_STRUCT()
struct CustomWorkspace {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char name[32] = "";
    LSD_PROPERTY(Edit) char base[32] = "";  // the built-in workspace it was made from (its editor area)
};

LSD_STRUCT()
struct EditorSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Startup", Tooltip = "Opened when the editor starts") char last_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Startup") List<RecentLevel, 10> recent_levels;
    LSD_PROPERTY(Edit, Category = "Startup", Units = "min", Min = 0, Tooltip = "Autosave unsaved levels to Saved/Autosaves (0 = off)") f32 autosave_minutes = 5.0f;
    LSD_PROPERTY(Edit, Category = "Viewport", Units = "m/s", Min = 0.1, Max = 100) f32 camera_speed = 6.0f;
    LSD_PROPERTY(Edit, Category = "Snapping") bool snap = true;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "m", Min = 0.01) f32 snap_move = 0.5f;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "deg", Min = 1) f32 snap_angle = 15.0f;
    LSD_PROPERTY(Edit, Category = "Snapping", Min = 0.01) f32 snap_scale = 0.25f;
    LSD_PROPERTY(Edit, Category = "Snapping", Tooltip = "Moving with the gizmo keeps the pivot on the surface under it (Unreal's Surface Snapping)") bool snap_to_surface = false;
    LSD_PROPERTY(Edit, Category = "Content Browser", Min = 48, Max = 256) f32 tile_size = 96.0f;
    LSD_PROPERTY(Edit, Category = "Content Browser") bool list_view = false;
    LSD_PROPERTY(Edit, Category = "Content Browser", Widget = "Choice:Name,Type,Date Modified,Size") u32 sort_by = 0;
    LSD_PROPERTY(Edit, Category = "Content Browser") List<FavouriteFolder, 32> favourites;
    LSD_PROPERTY(Edit, Category = "Content Browser") List<CollectionEntry, 512> collections;
    LSD_PROPERTY(Edit, Category = "Live Link", Tooltip = "A folder your DCC exports to (Blender, Maya: File > Export > FBX there). New and changed files are copied into Content/LiveLink and reload in the editor and Play; empty = off")
    char live_link_folder[260] = "";
    // Plugins (Editor Preferences > Plugins), not in the Settings list.
    LSD_PROPERTY(Save) bool mcp_enabled = false;  // Claude MCP: listen for tools/mcp/lsd_mcp on 127.0.0.1
    LSD_PROPERTY(Save) u32 mcp_port = 7780;
    // Workspace tabs under the menu bar (Blender's Layout / Modeling / Shading...).
    LSD_PROPERTY(Save) char workspace[32] = "Layout";
    LSD_PROPERTY(Save) List<CustomWorkspace, 8> workspaces;
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

// One entry of the project's gameplay tag list (Unreal's Gameplay Tag Manager), e.g. "Damage.Fire".
LSD_STRUCT()
struct GameplayTagDef {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char tag[64] = "";
    LSD_PROPERTY(Edit) char comment[96] = "";
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
    LSD_PROPERTY(Edit, Category = "Audio", Widget = "Asset:SoundMix", Tooltip = "Pushed when the game starts and never popped (Unreal's Base Sound Mix): the project's overall balance")
    char base_sound_mix[33] = "";
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Validate Assets also checks name prefixes (SM_, T_, M_, MI_, BT_, UI_...)") bool check_naming = false;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Textures larger than this on a side are reported") u32 max_texture_size = 4096;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Tooltip = "Tick for an online / LAN game. Off: a single-player game and nothing networked runs") bool multiplayer = false;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 1, Max = 16, Tooltip = "Play in the editor with this many players: the editor hosts (a listen server) and opens a game window for each other player") u32 play_players = 2;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 1024, Max = 65535) u32 port = 7777;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 2, Max = 256) u32 max_players = 16;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 10, Max = 120, Tooltip = "Updates a second: smoother with more, at more bandwidth") f32 tick_rate = 30.0f;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Units = "ms", Min = 0, Max = 500, Tooltip = "How far in the past clients show other objects: hides lag spikes and lost packets") f32 interpolation_ms = 100.0f;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Tooltip = "How each player looks to the others: a mesh asset or a shape (Cube, Sphere...) by name") char player_avatar[64] = "Cube";
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to everyone") bool chat_all = true;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to the player's team (scripts set teams: lsd.set_team)") bool chat_team = true;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to the player's squad inside the team") bool chat_squad = false;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to players near the speaker") bool chat_proximity = false;
    LSD_PROPERTY(Edit, Category = "Chat", Units = "m", Min = 1, Max = 1000) f32 proximity_range = 30.0f;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "The built-in chat box in Play (Enter to type); off: make your own with on_chat and lsd.chat") bool chat_box = true;
    LSD_PROPERTY(Edit, Category = "Gameplay Tags", Tooltip = "The tags the Details tag picker offers (dotted: Damage.Fire is under Damage)")
    List<GameplayTagDef, 128> gameplay_tags;
};

} // namespace lsd::editor
