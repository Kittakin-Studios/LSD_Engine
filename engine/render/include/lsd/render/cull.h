#pragma once
// CPU frustum tests on world-space bounds before draws are made, on rhi::frustum_planes' planes (the GPU cull
// pass's): a whole group of copies (an Instanced Static Mesh, a level's static meshes per shadow cascade) is kept or
// dropped with one box test, and single copies with rhi::instance_visible instead of eight projected corners.
#include "lsd/foundation/types.h"

namespace lsd::render {

// rhi::frustum_planes of `view_proj` with the near and far planes always passed: a shadow view's sides only
// (casters ahead of a cascade, toward the light, still cast into it).
void side_planes(const f32 view_proj[16], f32 planes[6][4]);

enum class Cull : u8 { Outside, Partly, Inside };
// A world box (lo..hi) against the planes.
Cull classify_box(const f32 planes[6][4], const f32 lo[3], const f32 hi[3]);

// A copy's local box (lo..hi) under its 3x4 `rows`: its world box grown into out_lo / out_hi, and the sphere
// around it (as screen_size measures it).
void grow_box(const f32 rows[12], const f32 lo[3], const f32 hi[3], f32 out_lo[3], f32 out_hi[3]);
void copy_sphere(const f32 rows[12], const f32 lo[3], const f32 hi[3], f32 centre[3], f32& radius);

} // namespace lsd::render
