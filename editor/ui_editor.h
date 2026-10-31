#pragma once
// The UI window (Unreal's Widget Blueprint editor): a UI document (.rml) or style sheet (.rcss) as
// text on the right, and on the left a designer canvas: a live preview where elements are clicked,
// dragged and added; their Details and drags are written into the same text (the text stays the
// source). The preview redraws 0.3 s after typing stops. Save writes the file; the game opens it
// from the next Play.
#include "editor_ui.h"

#include "lsd/asset/asset_registry.h"

#include <string>

namespace lsd::editor {

void open_ui_editor(EditorState& state, const asset::AssetRecord& record);
void draw_ui_editor(EditorState& state);
// The preview into its own target; between begin_frame and the viewports.
void render_ui_editor(rhi::Device& device, const f32 background[4]);
void shutdown_ui_editor();  // before ui::shutdown
// Creates <folder>/<base>.rml: a starter document (panel, text, a button with an action); returns its path.
std::string create_ui_document(const char* folder_path, const char* base);

} // namespace lsd::editor
