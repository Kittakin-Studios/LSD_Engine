#pragma once
// Workspaces (Blender's workspace tabs): Layout, Modeling, Shading, Animation, Scripting, UI, Effects
// and the user's own, each a remembered arrangement of the panels. Most have an editor area: asset
// editors opened while it is active dock there (the Material Graph under the viewport in Shading,
// the Timeline in Animation...). Layout is the editor's original arrangement and has none.

namespace lsd::editor {

// Call right before an asset editor's ImGui::Begin: it docks into the active workspace's editor area.
void dock_in_workspace();

} // namespace lsd::editor
