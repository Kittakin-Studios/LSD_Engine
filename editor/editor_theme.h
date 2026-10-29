#pragma once
// Editor colours. Reflected, so the Details panel edits them live and they save as a theme asset.
// Defaults follow the plan doc's layout, with its contrast fixes applied.
#include "lsd/reflect/color.h"

namespace lsd::editor {

LSD_STRUCT(Category = "Editor")
struct EditorTheme {
    LSD_GENERATED()

    // ---- Viewport ----
    LSD_PROPERTY(Edit, Category = "Viewport") LinearColor viewport_background = LinearColor::hex(0x0D0404);
    LSD_PROPERTY(Edit, Category = "Viewport") LinearColor grid_minor = LinearColor::hex(0x3A2A2A);
    LSD_PROPERTY(Edit, Category = "Viewport", Tooltip = "Every 5th grid line") LinearColor grid_major = LinearColor::hex(0x6E5A5A);
    LSD_PROPERTY(Edit, Category = "Viewport") LinearColor axis_x = LinearColor::hex(0xE53935);
    LSD_PROPERTY(Edit, Category = "Viewport") LinearColor axis_y = LinearColor::hex(0x43C743);
    LSD_PROPERTY(Edit, Category = "Viewport") LinearColor axis_z = LinearColor::hex(0x3D7BFF);

    // ---- Outline ----
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor outline_tab = LinearColor::hex(0x4A4E57);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor outline_row_a = LinearColor::hex(0x3C3838);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor outline_row_b = LinearColor::hex(0x242855);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor outline_scrollbar = LinearColor::hex(0xFFE51A);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor text_mesh = LinearColor::hex(0xE5E51A);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor text_component = LinearColor::hex(0x22E5FF);
    LSD_PROPERTY(Edit, Category = "Outline", Tooltip = "Contrast fix: was #0040FF") LinearColor text_blueprint = LinearColor::hex(0x5C8DFF);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor folder_row = LinearColor::hex(0xDDB97A);
    LSD_PROPERTY(Edit, Category = "Outline", Tooltip = "Contrast fix: dark text on the tan folder row") LinearColor text_folder = LinearColor::hex(0x3A2A00);
    LSD_PROPERTY(Edit, Category = "Outline") LinearColor vfx_row = LinearColor::hex(0x8F3A40);
    LSD_PROPERTY(Edit, Category = "Outline", Tooltip = "Contrast fix: light orange on the red VFX row") LinearColor text_vfx = LinearColor::hex(0xFFC266);

    // ---- Details ----
    LSD_PROPERTY(Edit, Category = "Details") LinearColor details_tab_active = LinearColor::hex(0xF3C312);
    LSD_PROPERTY(Edit, Category = "Details") LinearColor details_tab = LinearColor::hex(0x596CC1);
    LSD_PROPERTY(Edit, Category = "Details") LinearColor details_row_a = LinearColor::hex(0x596CC1);
    LSD_PROPERTY(Edit, Category = "Details") LinearColor details_row_b = LinearColor::hex(0x3C3838);

    // ---- Content Browser ----
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor content_tab = LinearColor::hex(0xEEA70E);
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor content_bar = LinearColor::hex(0x025291);
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor content_area = LinearColor::hex(0x2D2D30);
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor folder_tree_background = LinearColor::hex(0x000000);
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor folder_tree_border = LinearColor::hex(0x9BE11F);
    LSD_PROPERTY(Edit, Category = "Content Browser") LinearColor folder_tree_text = LinearColor::hex(0xFBF800);

    // ---- General ----
    LSD_PROPERTY(Edit, Category = "General") LinearColor panel_background = LinearColor::hex(0x1B1B1E);
    LSD_PROPERTY(Edit, Category = "General", Tooltip = "Text drawn on bright tabs (gold, orange)") LinearColor text_on_bright = LinearColor::hex(0x111111);
    LSD_PROPERTY(Edit, Category = "General", Min = 10, Max = 32, Units = "px") f32 font_size = 18.0f;
};

} // namespace lsd::editor
