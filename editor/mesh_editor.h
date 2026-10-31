#pragma once
// The Mesh Editor (Unreal's Static Mesh / Skeleton editors in one window): double-click a mesh. A
// preview you orbit by dragging (wheel: zoom), its numbers (triangles, LODs, size), its import
// settings with Save / Reimport; for rigged meshes the bone tree, bones drawn over the preview and
// sockets (named points on bones, saved in the .meta) to add and place.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

void open_mesh_editor(EditorState& state, const asset::AssetRecord& record);
void draw_mesh_editor(EditorState& state);
// The preview into its target; between begin_frame and the viewports.
void render_mesh_editor(rhi::Device& device, const f32 background[4]);
void shutdown_mesh_editor();  // before the device goes

} // namespace lsd::editor
