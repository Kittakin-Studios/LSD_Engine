#pragma once
// Texture import: PNG, JPG, TGA, BMP, PSD (stb_image) to RGBA8 with a full mip chain. Whether a
// texture is colour (sRGB) or data (linear) is decided by where it is used (a material's base
// colour slot vs its normal / ORM slots), which also decides how its mips are averaged.
#include "lsd/foundation/array.h"

namespace lsd::asset {

constexpr u32 kMaxTextureMips = 16;

struct TextureData {
    explicit TextureData(Allocator& allocator) : pixels(allocator) {}
    u32 width = 0, height = 0, mip_count = 0;
    Array<u8> pixels;                 // every mip, RGBA8, rows tightly packed
    usize mip_offset[kMaxTextureMips] = {};
    const u8* mip(u32 level) const { return pixels.data() + mip_offset[level]; }
};

// Reads an image, halves it until it fits `max_size`, and builds mips down to 1x1 (averaged in
// linear light when `srgb`). False if the file is missing or not an image.
bool load_texture(const char* path, bool srgb, u32 max_size, TextureData& out);
// The image's size from its header only (no decoding); false if it is not a readable image.
bool texture_size(const char* path, u32& width, u32& height);
// Same from RGBA8 pixels in memory (tests, generated textures).
bool build_texture(const u8* rgba, u32 width, u32 height, bool srgb, u32 max_size, TextureData& out);

} // namespace lsd::asset
