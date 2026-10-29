#pragma once
// Reflected settings edited live in the runtime's debug Details panel.
#include "lsd/reflect/color.h"

namespace lsd {

LSD_STRUCT(Category = "Debug")
struct ViewportSettings {
    LSD_GENERATED()

    LSD_PROPERTY(Edit, Category = "Viewport", Tooltip = "Colour the frame is cleared to")
    LinearColor background = LinearColor::hex(0x0D0404);

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
