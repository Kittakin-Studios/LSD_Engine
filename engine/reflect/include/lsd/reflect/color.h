#pragma once
// Reflected colour types shared by settings, themes and (later) materials and lights.
#include "lsd/reflect/reflect.h"

namespace lsd {

// RGBA in 0..1. Shown as a colour picker in the Details panel.
LSD_STRUCT(Color)
struct LinearColor {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 r = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 g = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 b = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 a = 1.0f;

    // From 0xRRGGBB, as written in the design docs.
    static constexpr LinearColor hex(u32 rgb, f32 alpha = 1.0f) {
        LinearColor c;
        c.r = static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f;
        c.g = static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f;
        c.b = static_cast<f32>(rgb & 0xFF) / 255.0f;
        c.a = alpha;
        return c;
    }
};

} // namespace lsd
