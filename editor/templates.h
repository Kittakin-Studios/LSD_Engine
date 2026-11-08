#pragma once
// Project templates in the editor: the template picker (the first time the editor opens with no
// level saved yet, or File > New from Template), the 2D template's starter files in
// Content/Templates/2D/ (a tileset, a player sprite sheet, a coin flipbook, a backdrop, the coin and
// player scripts and a HUD), the level saved and opened with the template's viewports and mode.
#include "editor_ui.h"
#include "template_levels.h"

namespace lsd::editor {

// Window > Viewports for the template's layout (kViewport2D / 2_5D / 3D bits).
u32 template_viewports(Template t);
// Builds the template into the editor: its art (2D), the level saved as Content/Levels/<name>.lsdlevel
// (a free name), then opened, with the template's viewports and Level mode.
void build_template(EditorState& s, Template t, const char* level_name);
// The template picker (a modal over the editor) while s.show_template_picker is set.
void draw_template_picker(EditorState& s);

} // namespace lsd::editor
