#pragma once
// Texture import: PNG, JPG, TGA, BMP, PSD, HDR (stb_image) and EXR (tinyexr) to RGBA8 with a full mip chain.
// HDR / EXR are float images: colour ones are encoded to sRGB, data ones (normal, ORM maps) kept linear,
// both clipped to 0..1 (shortcut: no float textures on the GPU yet; add RGBA16F when HDR skies / emissive need it). Whether a
// texture is colour (sRGB) or data (linear) is decided by where it is used (a material's base
// colour slot vs its normal / ORM slots), which also decides how its mips are averaged.
#include "lsd/foundation/array.h"

namespace lsd::asset {

constexpr u32 kMaxTextureMips = 16;

// How a TextureData's mips are stored: RGBA8 pixels, or BC1 / BC3 blocks (compress_texture).
enum class TextureEncoding : u8 { RGBA8, BC1, BC3 };

struct TextureData {
    explicit TextureData(Allocator& allocator) : pixels(allocator) {}
    u32 width = 0, height = 0, mip_count = 0;
    TextureEncoding encoding = TextureEncoding::RGBA8;
    Array<u8> pixels;                 // every mip, RGBA8, rows tightly packed
    usize mip_offset[kMaxTextureMips] = {};
    const u8* mip(u32 level) const { return pixels.data() + mip_offset[level]; }
};

// Reads an image, halves it until it fits `max_size`, and builds mips down to 1x1 (averaged in
// linear light when `srgb`). False if the file is missing or not an image.
bool load_texture(const char* path, bool srgb, u32 max_size, TextureData& out);
// Block-compresses the mips in place (stb_dxt, high quality): BC1 when every pixel is opaque, BC3 when any is not
// (4 / 8 bits a pixel instead of 32). False, and nothing changes, when the size is not a multiple of 4 or it is already.
bool compress_texture(TextureData& data);
// load_texture, then compress_texture when `compress`, the compressed result kept in `cache_dir` (DerivedData/Textures):
// later loads read it back while it matches the source (its write time and size) and max_size, instead of encoding again.
bool load_texture_cached(const char* path, bool srgb, u32 max_size, bool compress, const char* cache_dir, TextureData& out);
// The image's size from its header only (no decoding); false if it is not a readable image.
bool texture_size(const char* path, u32& width, u32& height);
// An image's grey (its first channel) at 16 bits, rows top to bottom (16-bit PNGs keep theirs; 8-bit ones are
// widened): heightmaps. False if it is not a readable image.
bool load_grey16(const char* path, u32& width, u32& height, Array<u16>& out);
// Any readable image as PNG bytes in memory, halved until its longer side fits `max_side` (a picture for a chat
// model). False if it is not a readable image.
bool encode_png(const char* path, u32 max_side, Array<u8>& png);
// RGBA8 pixels (rows top to bottom) to a PNG file.
bool write_png(const char* path, u32 width, u32 height, const u8* rgba);
// Same from RGBA8 pixels in memory (tests, generated textures).
bool build_texture(const u8* rgba, u32 width, u32 height, bool srgb, u32 max_size, TextureData& out);

} // namespace lsd::asset
