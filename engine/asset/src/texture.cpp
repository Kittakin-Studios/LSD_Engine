#include "lsd/asset/texture.h"

#include "lsd/foundation/log.h"

#include <stb_image.h>

#include <cmath>
#include <cstring>

namespace lsd::asset {

namespace {

// sRGB byte -> linear, and back (a table each way: mips of big textures touch millions of pixels).
struct SrgbTables {
    f32 to_linear[256];
    u8 to_srgb[4096];  // indexed by linear * 4095
    SrgbTables() {
        for (int i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            to_linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < 4096; ++i) {
            const f32 l = static_cast<f32>(i) / 4095.0f;
            const f32 c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            to_srgb[i] = static_cast<u8>(c * 255.0f + 0.5f);
        }
    }
};
const SrgbTables& srgb_tables() {
    static const SrgbTables tables;
    return tables;
}

// One 2x2 box-filtered level from the one above (odd edges repeat their last row / column).
void downsample(const u8* src, u32 w, u32 h, u8* dst, bool srgb) {
    const u32 dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    const SrgbTables& t = srgb_tables();
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            const u32 y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            const u8* p[4] = {src + (y0 * w + x0) * 4, src + (y0 * w + x1) * 4, src + (y1 * w + x0) * 4, src + (y1 * w + x1) * 4};
            u8* out = dst + (y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                if (srgb && c < 3) {  // average light, not bytes: keeps colour mips from darkening
                    const f32 sum = t.to_linear[p[0][c]] + t.to_linear[p[1][c]] + t.to_linear[p[2][c]] + t.to_linear[p[3][c]];
                    out[c] = t.to_srgb[static_cast<int>(sum * 0.25f * 4095.0f + 0.5f)];
                } else {
                    out[c] = static_cast<u8>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                }
            }
        }
    }
}

} // namespace

bool build_texture(const u8* rgba, u32 width, u32 height, bool srgb, u32 max_size, TextureData& out) {
    if (!rgba || width == 0 || height == 0) return false;
    // Halve until it fits (the extra levels are simply not kept).
    Array<u8> scratch(default_allocator());
    scratch.resize(static_cast<usize>(width) * height * 4);
    std::memcpy(scratch.data(), rgba, scratch.size());
    max_size = max_size ? max_size : 1;
    while (width > max_size || height > max_size) {
        Array<u8> smaller(default_allocator());
        const u32 w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1;
        smaller.resize(static_cast<usize>(w) * h * 4);
        downsample(scratch.data(), width, height, smaller.data(), srgb);
        scratch.clear();
        scratch.resize(smaller.size());
        std::memcpy(scratch.data(), smaller.data(), smaller.size());
        width = w;
        height = h;
    }
    out.width = width;
    out.height = height;
    out.mip_count = 0;
    usize total = 0;
    for (u32 w = width, h = height;; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
        out.mip_offset[out.mip_count++] = total;
        total += static_cast<usize>(w) * h * 4;
        if ((w == 1 && h == 1) || out.mip_count == kMaxTextureMips) break;
    }
    out.pixels.resize(total);
    std::memcpy(out.pixels.data(), scratch.data(), scratch.size());
    u32 w = width, h = height;
    for (u32 m = 1; m < out.mip_count; ++m) {
        downsample(out.pixels.data() + out.mip_offset[m - 1], w, h, out.pixels.data() + out.mip_offset[m], srgb);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return true;
}

bool load_texture(const char* path, bool srgb, u32 max_size, TextureData& out) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        LSD_LOG_WARN("asset", "Could not read image %s: %s", path, stbi_failure_reason());
        return false;
    }
    const bool ok = build_texture(pixels, static_cast<u32>(w), static_cast<u32>(h), srgb, max_size, out);
    stbi_image_free(pixels);
    return ok;
}

bool texture_size(const char* path, u32& width, u32& height) {
    int w = 0, h = 0, channels = 0;
    if (!stbi_info(path, &w, &h, &channels)) return false;
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
    return true;
}

} // namespace lsd::asset
