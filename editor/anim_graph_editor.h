#pragma once
// The State Machine window (Unreal's Anim Blueprint state machine): an AnimGraph data asset's states
// as boxes and its transitions as arrows on a canvas. Click a box or an arrow's dot to edit it beside
// the canvas; drag boxes to arrange them; right-click for Add State, Add Transition, Set as Entry,
// Delete (or the Delete key). In Play the state the first object using the graph is in glows.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

void open_anim_graph_editor(EditorState& state, const asset::AssetRecord& record);
void draw_anim_graph_editor(EditorState& state);

} // namespace lsd::editor
