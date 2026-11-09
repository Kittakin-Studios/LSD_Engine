#pragma once
// The Timeline window (Unreal's Sequencer, the basics): tracks on the left, their keys on a time
// ruler on the right. Drag the playhead to preview the level at that moment (Transform and Property
// tracks pose their objects; closing the window puts them back). Key adds a key at the playhead from
// the object as it is now (move it with the gizmo first), so animating is: scrub, move, Key.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

namespace lsd::editor {

void open_timeline_editor(EditorState& state, const asset::AssetRecord& record);
void draw_timeline_editor(EditorState& state);
// Movie Render Queue: opens the timeline and renders it at `fps`, `width` x `height`, one frame per
// editor frame, into Saved/MovieRenders/<name>/<name>_0000.png...
void render_timeline_movie(EditorState& state, const asset::AssetRecord& record, f32 fps, u32 width, u32 height);

} // namespace lsd::editor
