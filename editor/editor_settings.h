#pragma once
// Per-user editor settings, saved to Saved/editor_settings.json (like Unreal's EditorPerProjectUserSettings).
#include "lsd/reflect/reflect.h"

namespace lsd::editor {

LSD_STRUCT()
struct EditorSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Category = "Startup", Tooltip = "Opened when the editor starts") char last_level[260] = "";
    LSD_PROPERTY(Edit, Category = "Startup", Units = "min", Min = 0, Tooltip = "Autosave unsaved levels to Saved/Autosaves (0 = off)") f32 autosave_minutes = 5.0f;
    LSD_PROPERTY(Edit, Category = "Viewport", Units = "m/s", Min = 0.1, Max = 100) f32 camera_speed = 6.0f;
    LSD_PROPERTY(Edit, Category = "Snapping") bool snap = true;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "m", Min = 0.01) f32 snap_move = 0.5f;
    LSD_PROPERTY(Edit, Category = "Snapping", Units = "deg", Min = 1) f32 snap_angle = 15.0f;
    LSD_PROPERTY(Edit, Category = "Snapping", Min = 0.01) f32 snap_scale = 0.25f;
};

} // namespace lsd::editor
