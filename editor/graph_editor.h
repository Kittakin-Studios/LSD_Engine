#pragma once
// The graph window: Visual Scripts (Unreal's Blueprint event graph, compiled to Lua), Sound Cues and
// Material Graphs (compiled to a shader); one at a time, nodes and wires on a canvas, saved with Ctrl+S.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

#include <string>

namespace lsd::editor {

void open_graph(EditorState& state, const asset::AssetRecord& record);
void draw_graph_editor(EditorState& state);
void shutdown_graph_editor();  // frees the open graph and its preview (at exit)
// A Material Graph's preview (a turning sphere or cube) into its own target; between begin_frame
// and the viewports.
void render_graph_preview(EditorState& state, const f32 background[4]);
// Creates Content/<folder>/<base>.lsdgraph holding Event Start and Event Update; returns its path.
std::string create_graph_file(const char* folder_path, const char* base);
// Creates <folder>/<base>.lsdcue: a Wave Player wired to the Output; returns its path.
std::string create_cue_file(const char* folder_path, const char* base);
// Creates <folder>/<base>.lsdmatgraph: a Color wired to the Material Output's Base Color; returns its path.
std::string create_material_graph_file(const char* folder_path, const char* base);

// The Lua the game runs for a graph asset (Saved/Graphs/<guid>.lua), compiled again when the
// graph is newer. False (logged) if the graph does not compile.
bool graph_lua(const asset::AssetRegistry& registry, const asset::AssetRecord& record, char* lua_path, usize capacity);

} // namespace lsd::editor
