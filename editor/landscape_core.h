#pragma once
// Landscape logic without the GPU or the editor (shared with the tests): heightmap images in and
// out, and the holes cut into the terrain.
#include "lsd/world/components.h"

#include <vector>

namespace lsd::editor {

// Landscape tiles (a bigger world from several landscapes side by side): `to` becomes the tile beside `from`
// (dx / dy: -1, 0 or 1, one of them 0) with the same size, texture tiling and layers; its shared edge has `from`'s
// heights and every row across it continues them, so the seam is closed and the brushes sculpt both together.
void extend_tile(const world::Landscape& from, i32 dx, i32 dy, world::Landscape& to);
// A greyscale heightmap (RGBA8, its red channel: black 0, white max_height metres) sampled
// bilinearly onto the landscape's grid (heights resized to the full grid).
void heights_from_image(world::Landscape& landscape, const u8* rgba, u32 width, u32 height, f32 max_height);
// The heights as a greyscale image of the grid's size (kResolution square, RGBA8), 0..max_height to black..white.
void image_from_heights(const world::Landscape& landscape, f32 max_height, u8* rgba);
// The same from 16-bit greys (0..65535: 0..max_height), e.g. Unreal's .r16 / 16-bit PNG heightmaps, rows top to bottom.
void heights_from_samples(world::Landscape& landscape, const u16* samples, u32 width, u32 height, f32 max_height);
// The heights as 16-bit greys of the grid's size (kResolution square, top row = the +Y edge).
void samples_from_heights(const world::Landscape& landscape, f32 max_height, u16* samples);
// Unreal's RAW heightmap: square, little-endian 16-bit, no header (.r16 / .raw). False if it is not square.
bool read_r16(const char* path, std::vector<u16>& samples, u32& side);
bool write_r16(const char* path, const u16* samples, u32 side);
// Holes (Unreal's Landscape Visibility tool): a grid point marked as a hole removes the squares that touch it.
bool is_hole(const world::Landscape& landscape, u32 i, u32 j);
void set_hole(world::Landscape& landscape, u32 i, u32 j, bool hole);
// The square between points (i, j) and (i + 1, j + 1) is drawn and collides.
bool quad_open(const world::Landscape& landscape, u32 i, u32 j);

} // namespace lsd::editor
