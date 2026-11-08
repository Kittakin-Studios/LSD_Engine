#include "landscape_core.h"

#include <cmath>
#include <cstdio>

namespace lsd::editor {

namespace {
constexpr u32 kGrid = world::Landscape::kResolution;

// Samples an image (0..1 per pixel through `red`) bilinearly onto the grid.
template <typename Red>
void heights_from(world::Landscape& l, u32 width, u32 height, f32 max_height, Red red) {
    while (l.heights.count < kGrid * kGrid) l.heights.push_back(0.0f);
    if (!width || !height) return;
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

} // namespace

void heights_from_image(world::Landscape& l, const u8* rgba, u32 width, u32 height, f32 max_height) {
    if (!rgba) return heights_from(l, 0, 0, max_height, [](u32, u32) { return 0.0f; });
    heights_from(l, width, height, max_height, [&](u32 x, u32 y) { return static_cast<f32>(rgba[(static_cast<usize>(y) * width + x) * 4]) / 255.0f; });
}

void heights_from_samples(world::Landscape& l, const u16* samples, u32 width, u32 height, f32 max_height) {
    if (!samples) return heights_from(l, 0, 0, max_height, [](u32, u32) { return 0.0f; });
    heights_from(l, width, height, max_height, [&](u32 x, u32 y) { return static_cast<f32>(samples[static_cast<usize>(y) * width + x]) / 65535.0f; });
}

void samples_from_heights(const world::Landscape& l, f32 max_height, u16* samples) {
    for (u32 j = 0; j < kGrid; ++j) {
        for (u32 i = 0; i < kGrid; ++i) {
            const u32 k = j * kGrid + i;
            const f32 h = k < l.heights.count && max_height > 0.0f ? l.heights[k] / max_height : 0.0f;
            samples[static_cast<usize>(kGrid - 1 - j) * kGrid + i] = static_cast<u16>(h <= 0.0f ? 0 : h >= 1.0f ? 65535 : static_cast<u32>(h * 65535.0f + 0.5f));
        }
    }
}

bool read_r16(const char* path, std::vector<u16>& samples, u32& side) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<u8> bytes;
    u8 buffer[65536];
    for (usize n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(f);
    const usize count = bytes.size() / 2;
    side = static_cast<u32>(std::sqrt(static_cast<f64>(count)) + 0.5);
    if (bytes.size() % 2 || side < 2 || static_cast<usize>(side) * side != count) return false;
    samples.resize(count);
    for (usize i = 0; i < count; ++i) samples[i] = static_cast<u16>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));  // little-endian
    return true;
}

bool write_r16(const char* path, const u16* samples, u32 side) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::vector<u8> bytes(static_cast<usize>(side) * side * 2);
    for (usize i = 0; i < static_cast<usize>(side) * side; ++i) {
        bytes[i * 2] = static_cast<u8>(samples[i] & 0xFF);
        bytes[i * 2 + 1] = static_cast<u8>(samples[i] >> 8);
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
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

void extend_tile(const world::Landscape& from, i32 dx, i32 dy, world::Landscape& to) {
    constexpr u32 n = world::Landscape::kResolution;
    to.size = from.size;
    to.uv_tile = from.uv_tile;
    to.layers = from.layers;
    to.heights.count = n * n;
    auto height = [&](u32 i, u32 j) { return from.heights.count == n * n ? from.heights[j * n + i] : 0.0f; };
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            // The shared edge of `from`: its last column for a tile to +X, first for -X, likewise rows for Y.
            const u32 ei = dx > 0 ? n - 1 : dx < 0 ? 0 : i, ej = dy > 0 ? n - 1 : dy < 0 ? 0 : j;
            to.heights[j * n + i] = height(ei, ej);
        }
    }
}

} // namespace lsd::editor
