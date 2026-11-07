#include "landscape_core.h"

namespace lsd::editor {

namespace {
constexpr u32 kGrid = world::Landscape::kResolution;
}

void heights_from_image(world::Landscape& l, const u8* rgba, u32 width, u32 height, f32 max_height) {
    while (l.heights.count < kGrid * kGrid) l.heights.push_back(0.0f);
    if (!rgba || !width || !height) return;
    auto red = [&](u32 x, u32 y) { return static_cast<f32>(rgba[(static_cast<usize>(y) * width + x) * 4]) / 255.0f; };
    for (u32 j = 0; j < kGrid; ++j) {
        for (u32 i = 0; i < kGrid; ++i) {
            // Grid row j runs along +Y; image rows run downwards, so the image's top is the landscape's +Y edge.
            const f32 fx = static_cast<f32>(i) / static_cast<f32>(kGrid - 1) * static_cast<f32>(width - 1);
            const f32 fy = (1.0f - static_cast<f32>(j) / static_cast<f32>(kGrid - 1)) * static_cast<f32>(height - 1);
            const u32 x0 = static_cast<u32>(fx), y0 = static_cast<u32>(fy);
            const u32 x1 = x0 + 1 < width ? x0 + 1 : x0, y1 = y0 + 1 < height ? y0 + 1 : y0;
            const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
            const f32 top = red(x0, y0) + (red(x1, y0) - red(x0, y0)) * tx, bottom = red(x0, y1) + (red(x1, y1) - red(x0, y1)) * tx;
            l.heights[j * kGrid + i] = (top + (bottom - top) * ty) * max_height;
        }
    }
}

void image_from_heights(const world::Landscape& l, f32 max_height, u8* rgba) {
    for (u32 j = 0; j < kGrid; ++j) {
        for (u32 i = 0; i < kGrid; ++i) {
            const u32 k = j * kGrid + i;
            const f32 h = k < l.heights.count && max_height > 0.0f ? l.heights[k] / max_height : 0.0f;
            const u8 v = static_cast<u8>(h <= 0.0f ? 0 : h >= 1.0f ? 255 : static_cast<u32>(h * 255.0f + 0.5f));
            u8* p = rgba + (static_cast<usize>(kGrid - 1 - j) * kGrid + i) * 4;  // +Y at the top, as imported
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
}

bool is_hole(const world::Landscape& l, u32 i, u32 j) {
    const u32 k = j * kGrid + i;
    return i < kGrid && j < kGrid && k < l.holes.count && l.holes[k] != 0;
}

void set_hole(world::Landscape& l, u32 i, u32 j, bool hole) {
    if (i >= kGrid || j >= kGrid) return;
    if (!hole && !l.holes.count) return;
    while (l.holes.count < kGrid * kGrid) l.holes.push_back(0u);  // made on the first hole
    l.holes[j * kGrid + i] = hole ? 1u : 0u;
}

bool quad_open(const world::Landscape& l, u32 i, u32 j) {
    return !is_hole(l, i, j) && !is_hole(l, i + 1, j) && !is_hole(l, i, j + 1) && !is_hole(l, i + 1, j + 1);
}

} // namespace lsd::editor
