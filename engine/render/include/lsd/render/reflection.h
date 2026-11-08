#pragma once
// Reflection captures (Unreal's Sphere Reflection Capture) without cubemaps: a capture renders its
// surroundings into six square faces, read back, unwrapped here into one latitude-longitude tile
// of a shared atlas (2 x 4 tiles), with a mip chain so rougher surfaces sample blurrier levels. The
// lit shader picks the capture a pixel is inside and samples its tile along the reflection.
// Colours stay in the scene target's encoding (sqrt(c / (1 + c)) of exposed colour); mips average
// them decoded.
#include "lsd/foundation/math.h"

#include <vector>

namespace lsd::render {

constexpr u32 kReflectionFace = 128;                            // each captured face, square
constexpr u32 kReflectionTileW = 256, kReflectionTileH = 128;  // one capture in the atlas
constexpr u32 kReflectionAtlas = 512;                           // 2 columns x 4 rows of tiles
constexpr u32 kMaxReflections = 8;
constexpr u32 kReflectionMips = 6;                              // 512 down to 16 (a tile is 8 x 4 there)

// The six view-projections a capture renders from `eye`: +X, -X, +Y, -Y, +Z, -Z, 90 degrees, square.
void reflection_views(Vec3 eye, Mat4 out[6]);
// Unwraps six faces (RGBA8, rows top to bottom, kReflectionFace square, rendered with
// reflection_views) into tile `index` of `atlas` (RGBA8, kReflectionAtlas square). Tile texel (u, v):
// longitude u * 2pi - pi around +Z from +X, v = 0 straight up (+Z), v = 1 straight down.
void unwrap_reflection(const u8* const faces[6], const Mat4 views[6], u32 index, u8* atlas);
// An HDRI (latitude-longitude, half-float RGBA rows top to bottom: straight up first, its middle column along +X, the
// same layout as a tile) resampled into tile `index`, times `scale`, stored like a capture (as it shows on screen).
// `averages` gets its colour overhead (within 30 degrees of straight up), at the horizon (the 15 degrees above it) and
// below the horizon, unscaled.
void hdri_to_tile(const u16* half_rgba, u32 width, u32 height, f32 scale, u32 index, u8* atlas, Vec3 averages[3]);
// Levels 1.. of the atlas's mip chain (each half the last), averaged in decoded colour.
void reflection_mips(const u8* atlas, std::vector<std::vector<u8>>& levels);

} // namespace lsd::render
