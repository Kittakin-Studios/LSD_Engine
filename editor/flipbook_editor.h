#pragma once
// The Flipbook window (Unreal's Paper2D Flipbook Editor): double-click a Flipbook asset. The
// animation plays at the top; below, its preview sprite sheet with a grid (click a cell to add it as
// a frame) and the frame list (cell, duration, event; move up / down, delete). Sprites using the
// flipbook pick up a save at once.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

void open_flipbook_editor(EditorState& state, const asset::AssetRecord& record);
void draw_flipbook_editor(EditorState& state);

} // namespace lsd::editor
