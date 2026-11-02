#pragma once
// Panel tabs and workspaces. The top row is Unreal's document tabs: the Level, then each asset editor
// opened from Layout (or a workspace without an editor area), full screen. Under it, only on the Level
// tab, the workspace row (Blender's workspace tabs): Layout, Modeling, Shading, Animation, Scripting,
// UI, Effects and the user's own, each a remembered arrangement of the level's panels. Most have an
// editor area: asset editors opened while it is active dock there instead (the Material Graph under
// the viewport in Shading, the Timeline in Animation...).

namespace lsd::editor {

// An asset editor's ImGui::Begin: as a panel tab, or into the active workspace's editor area. Returns
// false when it is hidden (End already called); `*open` turns false when its tab is closed.
bool begin_editor(const char* title, bool* open);

} // namespace lsd::editor
