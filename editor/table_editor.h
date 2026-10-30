#pragma once
// The Data Table window: a .csv asset as an editable grid (Unreal's Data Table editor). The first
// column holds row names, the first row column names.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

#include <string>

namespace lsd::editor {

void open_table(EditorState& state, const asset::AssetRecord& record);
void draw_table_editor(EditorState& state);
// Creates <folder>/<base>.csv with a Name and a Value column and one row; returns its path.
std::string create_table_file(const char* folder_path, const char* base);

} // namespace lsd::editor
