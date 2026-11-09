#pragma once
// Editor Utility scripts and widgets in the editor (Unreal's Editor Utility Blueprints / Widgets / Scripted
// Actions): Run from the Content Browser's right-click, Tools > Editor Utilities, the toolbar's Utilities
// menu, the Outliner's Scripted Actions or `utility.Run <name>`; their panels dock like any editor window.
// The scripts' API is in editor_utility_core.h. A packaged game never builds the host (main.cpp: !game_mode).
#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

struct EditorState;

// A Lua script or visual script can be run as a utility; those named EU_* are listed in the menus.
bool can_run_utility(const asset::AssetRecord& record);
bool is_editor_utility(const asset::AssetRecord& record);
// Runs it on the edited level (not while playing); what it changes is one undo step.
bool run_editor_utility(EditorState& s, const asset::AssetRecord& record);
// MenuItems for every EU_ utility (Tools, toolbar, Outliner); "(none)" when there are none.
void draw_utility_menu_items(EditorState& s);
void draw_utility_panels(EditorState& s);
void register_utility_commands(EditorState& s);  // utility.Run <name>

} // namespace lsd::editor
