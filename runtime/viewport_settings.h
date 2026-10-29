#pragma once
// Reflected settings edited live in the runtime's debug Details panel.
#include "lsd/reflect/reflect.h"

namespace lsd {

LSD_STRUCT()
struct LinearColor {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 r = 0.051f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 g = 0.016f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 b = 0.016f;
};

LSD_STRUCT(Category = "Debug")
struct ViewportSettings {
    LSD_GENERATED()

    LSD_PROPERTY(Edit, Category = "Viewport", Tooltip = "Colour the frame is cleared to")
    LinearColor background;

    LSD_PROPERTY(Edit, Category = "Triangle")
    bool draw_triangle = true;

    LSD_PROPERTY(Edit, Category = "Triangle", Min = 0.1, Max = 2)
    f32 scale = 1.0f;

    LSD_PROPERTY(Edit, Category = "Triangle", Units = "deg/s", Min = -720, Max = 720, Tooltip = "Rotation speed; negative spins clockwise")
    f32 spin_speed = 45.0f;

    LSD_PROPERTY(Visible, Category = "Stats", Units = "fps")
    f32 frame_rate = 0.0f;

    LSD_PROPERTY(Visible, Category = "Stats", Units = "MiB")
    u64 vram_used = 0;

    LSD_PROPERTY(Visible, Category = "Stats", Units = "MiB")
    u64 vram_budget = 0;
};

} // namespace lsd
