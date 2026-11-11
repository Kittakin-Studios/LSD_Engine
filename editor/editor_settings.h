#pragma once
// Per-user editor settings, saved to Saved/editor_settings.json (like Unreal's EditorPerProjectUserSettings).
#include "lsd/reflect/color.h"
#include "lsd/reflect/reflect.h"

namespace lsd::editor {

// A folder starred in the Content Browser.
LSD_STRUCT()
struct FavouriteFolder {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "A Content folder pinned to the top of the folder tree") char path[256] = "";
};

// One asset in a collection (Unreal's Collections): a named set of assets from any folders.
LSD_STRUCT()
struct CollectionEntry {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The collection this asset belongs to") char collection[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "The asset in the collection (its permanent ID)") char guid[33] = "";
};

// A level opened or saved lately (File > Recent Levels), newest first.
LSD_STRUCT()
struct RecentLevel {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "A level opened recently (File > Recent Levels)") char path[260] = "";
};

// A workspace tab the user added with + (a copy of another workspace's arrangement).
LSD_STRUCT()
struct CustomWorkspace {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The workspace tab's name") char name[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "The built-in workspace it was made from (decides where asset editors open)") char base[32] = "";  // the built-in workspace it was made from (its editor area)
};

// One Foliage mode type (Unreal's Foliage Type): a mesh and how it is scattered by the brush.
LSD_STRUCT()
struct FoliageType {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Painted with the others when ticked") bool enabled = true;
    LSD_PROPERTY(Edit, Tooltip = "The mesh it scatters") char mesh[40] = "Sphere";
    LSD_PROPERTY(Edit, Tooltip = "Colour of its copies") LinearColor color = LinearColor::hex(0x4F8A3C);
    LSD_PROPERTY(Edit, Min = 0.01, Max = 20, Tooltip = "Copies per square metre a stroke puts down") f32 density = 1.0f;
    LSD_PROPERTY(Edit, Min = 0.05, Max = 10, Tooltip = "Smallest random size") f32 scale_min = 0.8f;
    LSD_PROPERTY(Edit, Min = 0.05, Max = 10, Tooltip = "Largest random size") f32 scale_max = 1.2f;
    LSD_PROPERTY(Edit, Tooltip = "A random turn around Z for each copy") bool random_yaw = true;
    LSD_PROPERTY(Edit, Tooltip = "Tilt each copy to the surface it lands on (grass on slopes); off: always upright (trees)") bool align_to_normal = false;
    LSD_PROPERTY(Edit, Tooltip = "Its copies block in Play (trees, rocks); off for grass and flowers") bool collision = false;
    LSD_PROPERTY(Save, Tooltip = "The Foliage Type asset it came from (Save to Asset writes it back): shared with the team") char asset[33] = "";
};

// A keyboard shortcut the user changed (Editor Preferences > Keyboard Shortcuts); keys "" = none.
LSD_STRUCT()
struct ShortcutOverride {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The editor command (keymap.h)") char command[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Its keys, e.g. Ctrl+Shift+S") char keys[32] = "";
};

LSD_STRUCT()
struct EditorSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Startup", Tooltip = "Opened when the editor starts") char last_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Startup", Tooltip = "Levels opened recently, newest first (File > Recent Levels)") List<RecentLevel, 10> recent_levels;
    LSD_PROPERTY(Edit, Category = "Startup", Units = "min", Min = 0, Tooltip = "Autosave unsaved levels to Saved/Autosaves (0 = off)") f32 autosave_minutes = 5.0f;
    LSD_PROPERTY(Edit, Category = "Viewport", Units = "m/s", Min = 0.1, Max = 100, Tooltip = "How fast the viewport camera flies with WASD (the mouse wheel changes it while flying)") f32 camera_speed = 6.0f;
    LSD_PROPERTY(Edit, Category = "Snapping", Tooltip = "Moves, turns and scales with the gizmo go in steps (the toolbar's Snap button)") bool snap = true;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "m", Min = 0.01, Tooltip = "Move step with Snap on") f32 snap_move = 0.5f;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "deg", Min = 1, Tooltip = "Turn step with Snap on") f32 snap_angle = 15.0f;
    LSD_PROPERTY(Edit, Category = "Snapping", Min = 0.01, Tooltip = "Scale step with Snap on") f32 snap_scale = 0.25f;
    LSD_PROPERTY(Edit, Category = "Snapping", Tooltip = "Moving with the gizmo keeps the pivot on the surface under it (Unreal's Surface Snapping)") bool snap_to_surface = false;
    LSD_PROPERTY(Edit, Category = "Content Browser", Min = 48, Max = 256, Tooltip = "Size of the Content Browser's asset tiles") f32 tile_size = 96.0f;
    LSD_PROPERTY(Edit, Category = "Content Browser", Tooltip = "The Content Browser shows assets as a list (name, type, size) instead of tiles") bool list_view = false;
    LSD_PROPERTY(Edit, Category = "Content Browser", Widget = "Choice:Name,Type,Date Modified,Size", Tooltip = "Order of assets in the Content Browser") u32 sort_by = 0;
    LSD_PROPERTY(Edit, Category = "Content Browser", Tooltip = "Folders pinned to the top of the Content Browser's tree (right-click a folder > Add to Favourites)") List<FavouriteFolder, 32> favourites;
    LSD_PROPERTY(Edit, Category = "Content Browser", Tooltip = "Your Collections: groups of assets from any folder (right-click an asset > Add to Collection)") List<CollectionEntry, 512> collections;
    LSD_PROPERTY(Edit, Category = "Live Link", Tooltip = "A folder your DCC exports to (Blender, Maya: File > Export > FBX there). New and changed files are copied into Content/LiveLink and reload in the editor and Play; empty = off")
    char live_link_folder[260] = "";
    // Plugins (Editor Preferences > Plugins), not in the Settings list.
    LSD_PROPERTY(Save) bool mcp_enabled = false;  // Claude MCP: listen for tools/mcp/lsd_mcp on 127.0.0.1
    LSD_PROPERTY(Save) u32 mcp_port = 7780;
    // LSD Assistant (Tools > LSD Assistant): the model it talks to. Saved per user in Saved/ (never in
    // the project or Git); an empty key falls back to ANTHROPIC_API_KEY / OPENAI_API_KEY.
    LSD_PROPERTY(Save) bool assistant_enabled = true;
    LSD_PROPERTY(Save) u32 assistant_provider = 0;  // 0 Anthropic (Claude), 1 OpenAI-compatible (OpenAI, OpenRouter, Ollama, LM Studio...)
    LSD_PROPERTY(Save) char assistant_endpoint[256] = "";  // empty: the provider's default
    LSD_PROPERTY(Save) char assistant_model[96] = "claude-opus-5-5";
    LSD_PROPERTY(Save) char assistant_key[256] = "";
    LSD_PROPERTY(Save) u32 assistant_effort = 1;  // Claude: 0 low, 1 medium, 2 high
    LSD_PROPERTY(Save) u32 assistant_role = 0;    // which studio role answers (assistant.cpp's list)
    LSD_PROPERTY(Save) u32 assistant_tools = 1;   // 0 off, 1 ask before each tool call, 2 allowed
    // Workspace tabs under the menu bar (Blender's Layout / Modeling / Shading...).
    LSD_PROPERTY(Save) char workspace[32] = "Layout";
    LSD_PROPERTY(Save) List<CustomWorkspace, 8> workspaces;
    LSD_PROPERTY(Save) List<ShortcutOverride, 64> shortcuts;  // Editor Preferences > Keyboard Shortcuts
    LSD_PROPERTY(Save) u32 viewport_kinds = 0;  // Window > Viewports (2D 1, 2.5D 2, 3D 4); 0 = the usual single 3D view
    LSD_PROPERTY(Save) List<FoliageType, 8> foliage_types;    // Foliage mode's list (none: one Sphere type)
};

// A key the player rebound: in every mapping of `action` (an Input Action by asset name; Move, Look and
// Jump for the built-in controls), `from` (the key the mapping context has) is replaced by `to`.
LSD_STRUCT()
struct KeyBinding {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The Input Action whose key the player changed") char action[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "The key the project set") char from[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "The key the player chose instead") char to[32] = "";
};

// Device Profile (Unreal's Device Profiles): a project rule for some PCs, applied over the recommended quality the
// first time the game runs there (and by Settings > Auto-detect). New Data Asset > DeviceProfile; profiles are
// tried by name, the first that matches wins.
LSD_STRUCT(Category = "Project", Tooltip = "Graphics quality for some PCs (a GPU, an amount of video memory, a platform)")
struct DeviceProfile {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Match", Tooltip = "The graphics card's name contains this (\"Steam Deck\", \"RTX 40\", \"Intel\"); empty = any card") char gpu_contains[64] = "";
    LSD_PROPERTY(Edit, Category = "Match", Units = "MB", Tooltip = "At least this much video memory (0 = any)") u32 min_video_memory_mb = 0;
    LSD_PROPERTY(Edit, Category = "Match", Units = "MB", Tooltip = "At most this much video memory (0 = any)") u32 max_video_memory_mb = 0;
    LSD_PROPERTY(Edit, Category = "Match", Widget = "Choice:Any,Windows,Linux", Tooltip = "Only on this platform") u32 platform = 0;
    LSD_PROPERTY(Edit, Category = "Quality", Widget = "Choice:Low,Medium,High,Epic", Tooltip = "The overall quality it starts with") u32 quality = 2;
    LSD_PROPERTY(Edit, Category = "Quality", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "Shadows (Overall: follows the quality)") u32 shadow_quality = 0;
    LSD_PROPERTY(Edit, Category = "Quality", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "Post Processing") u32 post_quality = 0;
    LSD_PROPERTY(Edit, Category = "Quality", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "View Distance") u32 view_distance_quality = 0;
    LSD_PROPERTY(Edit, Category = "Quality", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "Effects") u32 effects_quality = 0;
};

// The player's own settings (Unreal's Game User Settings), saved to Saved/GameUserSettings.json and
// applied at once when changed: the game's options menu edits them from Lua (lsd.get_settings /
// lsd.set_settings). The first run picks the quality the graphics card suits.
LSD_STRUCT()
struct GameUserSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Graphics", Tooltip = "The game fills the screen (off: a window)") bool fullscreen = false;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Low,Medium,High,Epic", Tooltip = "Overall quality (Unreal's Engine Scalability): every group below that is on Overall follows it")
    u32 quality = 2;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "Shadows: Low none, Medium the sun's only and nearer, High as set, Epic further")
    u32 shadow_quality = 0;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Overall,Low,Medium,High,Epic",
                 Tooltip = "Post Processing: Low bloom, tonemap, grading and FXAA only; Medium adds ambient occlusion; High and Epic everything the volume sets (reflections, depth of field, motion blur, contact shadows, TAA)")
    u32 post_quality = 0;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "View Distance: draw and cull distances (and where LODs switch) times 0.5, 0.75, 1 or 1.5")
    u32 view_distance_quality = 0;
    LSD_PROPERTY(Edit, Category = "Graphics", Widget = "Choice:Overall,Low,Medium,High,Epic", Tooltip = "Effects: particles spawned times 0.25 (Low), 0.5 (Medium) or 1")
    u32 effects_quality = 0;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1, Tooltip = "Loudness of everything the game plays (0 = silent)") f32 master_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1, Tooltip = "The Sound Class named Music") f32 music_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Audio", Min = 0, Max = 1, Tooltip = "The Sound Class named SFX") f32 effects_volume = 1.0f;
    LSD_PROPERTY(Edit, Category = "Controls", Min = 0.1, Max = 5, Tooltip = "Times the Character's look sensitivity") f32 mouse_sensitivity = 1.0f;
    LSD_PROPERTY(Edit, Category = "Controls", Tooltip = "Moving the mouse up looks down (flight controls)") bool invert_look = false;
    LSD_PROPERTY(Edit, Category = "Controls", Tooltip = "Keys the player rebound (lsd.rebind)") List<KeyBinding, 32> key_bindings;
    LSD_PROPERTY(Edit, Category = "Language", Tooltip = "A column of the project's string table (en, fr...); empty: the project's default") char language[16] = "";
    LSD_PROPERTY(Edit, Category = "Accessibility", Min = 0.5, Max = 3, Tooltip = "Times the size of all game UI text (menus, HUD, subtitles, world panels); 1 = as designed")
    f32 ui_text_scale = 1.0f;
    LSD_PROPERTY(Edit, Category = "Accessibility", Tooltip = "Sounds with a Subtitle show it at the bottom of the screen while they play") bool subtitles = true;
    LSD_PROPERTY(Edit, Category = "Accessibility", Widget = "Choice:Off,Deuteranopia,Protanopia,Tritanopia",
                 Tooltip = "Colour-blind filter (Unreal's Color Deficiency): Deuteranopia (green-weak), Protanopia (red-weak), Tritanopia (blue-weak)")
    u32 color_deficiency = 0;
    LSD_PROPERTY(Edit, Category = "Accessibility", Min = 0, Max = 1, Tooltip = "How strong the colour-blind filter is (0 = none, 1 = full)") f32 color_deficiency_strength = 1.0f;
    LSD_PROPERTY(Edit, Category = "Accessibility", Tooltip = "Show how the game looks to that colour-blindness instead of correcting for it (for checking your colours)")
    bool color_deficiency_simulate = false;
};

// One entry of the project's gameplay tag list (Unreal's Gameplay Tag Manager), e.g. "Damage.Fire".
LSD_STRUCT()
struct GameplayTagDef {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "A tag name, with dots for its family: Enemy.Boss, Damage.Fire") char tag[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "What the tag means (shown when picking it)") char comment[96] = "";
};

// A global material parameter (Unreal's Material Parameter Collection entry): its name and starting value.
LSD_STRUCT()
struct MaterialParameterDef {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "What Global Parameter nodes and lsd.set_material_parameter call it (Wind, Wetness, SnowAmount...)") char name[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Its value until a script sets it (a number: its R)") LinearColor value = {1.0f, 1.0f, 1.0f, 1.0f};
};

// Project-wide settings shared by the team, saved to Config/project.json (Unreal's Project Settings).
LSD_STRUCT()
struct ProjectSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Description", Tooltip = "The game's name: the window title and the packaged .exe") char project_name[64] = "LSD Project";
    LSD_PROPERTY(Edit, Category = "Description", Tooltip = "Who makes it (shown in the packaged game's file details)") char company[64] = "Kittakin Studios";
    LSD_PROPERTY(Edit, Category = "Maps", Tooltip = "Level the packaged game opens first (Content/Levels/...)") char game_default_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Maps", Tooltip = "Level the editor opens when there is no last level") char editor_startup_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Input", Tooltip = "Esc ends the packaged game. Off: Esc is the game's key (scripts see \"Escape\", e.g. a pause menu) and the game ends with lsd.quit; in the editor Esc still stops Play")
    bool escape_quits_game = true;
    LSD_PROPERTY(Edit, Category = "Platform", Widget = "Asset:Texture", Tooltip = "The game's icon: the window, the taskbar and the packaged .exe in Explorer (a square PNG, 256 x 256 is plenty)") char icon[33] = "";
    LSD_PROPERTY(Edit, Category = "Platform", Widget = "Asset:Texture", Tooltip = "Splash screen: shown while the packaged game starts (loading the level), centred, at its own size up to 1280 px. Empty: none")
    char splash[33] = "";
    LSD_PROPERTY(Edit, Category = "Platform", Units = "s", Min = 0, Max = 10, Tooltip = "The splash stays at least this long, then goes once the game is playing") f32 splash_seconds = 2.0f;
    LSD_PROPERTY(Edit, Category = "Platform", Tooltip = "Steam: written to steam_appid.txt next to the packaged exe (0 = not on Steam)") u32 steam_app_id = 0;
    LSD_PROPERTY(Edit, Category = "Packaging",
                 Tooltip = "Cook (Unreal's): the package gets only the assets the game uses, found from these settings (Game Default Level, Game Mode...) through references and through names in scripts and UI, with their compiled data so nothing compiles on first run. What was left out is listed in cook_report.txt. Off: all of Content/")
    bool cook_used_only = false;
    LSD_PROPERTY(Edit, Category = "Packaging", Widget = "Choice:Development,Shipping",
                 Tooltip = "Development: the package takes -remote (the editor's Remote Console), -exec and test commands, for testing on other PCs. Shipping: what players get; those are ignored (logs and crash reports stay)")
    u32 build_configuration = 0;
    LSD_PROPERTY(Edit, Category = "Localization", Widget = "Asset:Table", Tooltip = "String table: a Data Table with Key, then one column per language (en, fr...)") char string_table[33] = "";
    LSD_PROPERTY(Edit, Category = "Localization", Tooltip = "The language the game starts in (a column of the string table)") char default_language[16] = "en";
    LSD_PROPERTY(Edit, Category = "Audio", Widget = "Asset:SoundMix", Tooltip = "Pushed when the game starts and never popped (Unreal's Base Sound Mix): the project's overall balance")
    char base_sound_mix[33] = "";
    LSD_PROPERTY(Edit, Category = "User Interface", Units = "%", Min = 0, Max = 20,
                 Tooltip = "Safe zone (Unreal's Display Safe Zone): the game UI is laid out this far in from every edge of the view, so TVs that crop the picture keep it whole (5 is usual for TVs, 0 for monitors)")
    f32 ui_safe_zone = 0.0f;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Validate Assets also checks name prefixes (SM_, T_, M_, MI_, BT_, UI_...)") bool check_naming = false;
    LSD_PROPERTY(Edit, Category = "Validation", Tooltip = "Textures larger than this on a side are reported") u32 max_texture_size = 4096;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Tooltip = "Tick for an online / LAN game. Off: a single-player game and nothing networked runs") bool multiplayer = false;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 1, Max = 16, Tooltip = "Play in the editor with this many players: the editor hosts (a listen server) and opens a game window for each other player") u32 play_players = 2;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 1024, Max = 65535, Tooltip = "The network port the server listens on (players connect to it)") u32 port = 7777;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 2, Max = 256, Tooltip = "How many players one server takes, the host included") u32 max_players = 16;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Min = 10, Max = 120, Tooltip = "Updates a second: smoother with more, at more bandwidth") f32 tick_rate = 30.0f;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Units = "ms", Min = 0, Max = 500, Tooltip = "How far in the past clients show other objects: hides lag spikes and lost packets") f32 interpolation_ms = 100.0f;
    LSD_PROPERTY(Edit, Category = "Multiplayer", Tooltip = "How each player looks to the others: a mesh asset or a shape (Cube, Sphere...) by name") char player_avatar[64] = "Cube";
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to everyone") bool chat_all = true;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to the player's team (scripts set teams: lsd.set_team)") bool chat_team = true;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to the player's squad inside the team") bool chat_squad = false;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "Text chat to players near the speaker") bool chat_proximity = false;
    LSD_PROPERTY(Edit, Category = "Chat", Units = "m", Min = 1, Max = 1000, Tooltip = "Proximity chat: players further apart than this do not see each other's messages") f32 proximity_range = 30.0f;
    LSD_PROPERTY(Edit, Category = "Chat", Tooltip = "The built-in chat box in Play (Enter to type); off: make your own with on_chat and lsd.chat") bool chat_box = true;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Voice chat in multiplayer games (the microphone opens only when this is on)") bool voice_chat = true;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Talk to your team") bool voice_team = true;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Talk to everyone (the All key)") bool voice_all = false;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Talk to your squad inside the team") bool voice_squad = false;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Players near you hear you, quieter with distance (Chat > Proximity Range)") bool voice_proximity = false;
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Hold to talk on the first of team, proximity, squad, all that is on (SDL key name)") char push_to_talk_key[24] = "V";
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Hold to talk to everyone (when Voice All is on)") char talk_to_all_key[24] = "B";
    LSD_PROPERTY(Edit, Category = "Voice", Tooltip = "Always transmitting instead of push-to-talk") bool open_mic = false;
    LSD_PROPERTY(Edit, Category = "Gameplay Tags", Tooltip = "The tags the Details tag picker offers (dotted: Damage.Fire is under Damage)")
    List<GameplayTagDef, 128> gameplay_tags;
    LSD_PROPERTY(Edit, Category = "Materials",
                 Tooltip = "Global material parameters (Unreal's Material Parameter Collection): values every material can read (the Global Parameter node) and scripts set (lsd.set_material_parameter): wind, wetness, snow, time of day; up to 16")
    List<MaterialParameterDef, 16> material_parameters;
};

} // namespace lsd::editor
