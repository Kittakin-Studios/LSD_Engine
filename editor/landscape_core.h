#pragma once
// Landscape logic without the GPU or the editor (shared with the tests): heightmap images in and
// out, and the holes cut into the terrain.
#include "lsd/world/components.h"

namespace lsd::editor {

// A greyscale heightmap (RGBA8, its red channel: black 0, white max_height metres) sampled
// bilinearly onto the landscape's grid (heights resized to the full grid).
void heights_from_image(world::Landscape& landscape, const u8* rgba, u32 width, u32 height, f32 max_height);
// The heights as a greyscale image of the grid's size (kResolution square, RGBA8), 0..max_height to black..white.
void image_from_heights(const world::Landscape& landscape, f32 max_height, u8* rgba);
// Holes (Unreal's Landscape Visibility tool): a grid point marked as a hole removes the squares that touch it.
bool is_hole(const world::Landscape& landscape, u32 i, u32 j);
void set_hole(world::Landscape& landscape, u32 i, u32 j, bool hole);
// The square between points (i, j) and (i + 1, j + 1) is drawn and collides.
bool quad_open(const world::Landscape& landscape, u32 i, u32 j);

} // namespace lsd::editor
