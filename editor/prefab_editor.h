#pragma once
// The Prefab editor (Unreal's Blueprint editor viewport and components panel): double-click a prefab.
// It opens in a world of its own: a preview you orbit, its objects as a tree (add children, delete),
// the selected object's components (edit, add, remove). Save writes the .lsdprefab and updates every
// placed copy in the level (their overridden properties stay).
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

void open_prefab_editor(EditorState& state, const asset::AssetRecord& record);
void draw_prefab_editor(EditorState& state);
void render_prefab_editor(rhi::Device& device, const f32 background[4]);
void shutdown_prefab_editor();

} // namespace lsd::editor
