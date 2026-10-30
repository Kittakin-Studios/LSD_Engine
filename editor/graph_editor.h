#pragma once
// The Visual Script window (Unreal's Blueprint event graph): one .lsdgraph at a time, nodes and
// wires on a canvas, saved and compiled to Lua with Ctrl+S.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

#include <string>

namespace lsd::editor {

void open_graph(EditorState& state, const asset::AssetRecord& record);
void draw_graph_editor(EditorState& state);
// Creates Content/<folder>/<base>.lsdgraph holding Event Start and Event Update; returns its path.
std::string create_graph_file(const char* folder_path, const char* base);

// The Lua the game runs for a graph asset (Saved/Graphs/<guid>.lua), compiled again when the
// graph is newer. False (logged) if the graph does not compile.
bool graph_lua(const asset::AssetRegistry& registry, const asset::AssetRecord& record, char* lua_path, usize capacity);

} // namespace lsd::editor
