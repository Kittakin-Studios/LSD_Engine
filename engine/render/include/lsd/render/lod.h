#pragma once
// Levels of detail (Unreal's LOD screen sizes and auto LODs): which simpler version of a mesh to draw from how big
// it looks, how simple each automatic LOD is built, and an Instanced Static Mesh's copies grouped by the LOD each needs.
#include "lsd/render/mesh.h"

namespace lsd::render {

// LOD 0 (the mesh) and five simpler ones. The asset's LOD 1..3 Screen Sizes pick the first three; LOD 4 and 5 take
// over at half and a quarter of LOD 3's size (far away, where kit pieces of a few hundred triangles still add up).
constexpr u32 kMaxLods = 6;
// A 1080-pixel-high view with a 60 degree field of view: pixels per radian at the centre (screen sizes are radians).
constexpr f32 kPixelsPerRadian = 935.0f;

// The LOD a shadow map needs for a caster `radius` metres across drawn at `texel` metres per shadow texel: the one a
// 1080p view would pick if the caster covered that many pixels, never finer than `camera_lod` (what the camera sees).
// Detail finer than a texel cannot show in the shadow; far cascades' texels are tens of centimetres.
u32 shadow_lod(const f32 sizes[kMaxLods], u32 lod_count, u32 camera_lod, f32 radius, f32 texel);

// The screen sizes each LOD takes over below, from the asset's three (sizes[0] is never used).
void lod_screen_sizes(f32 lod1, f32 lod2, f32 lod3, f32 sizes[kMaxLods]);

// How big a box looks: its bounding radius under a copy's 3x4 `rows` over its distance from `eye`, times
// `distance_scale` (Engine Scalability's View Distance: above 1 everything is treated as nearer).
f32 screen_size(const f32 rows[12], const f32 lo[3], const f32 hi[3], const f32 eye[3], f32 distance_scale);

// The LOD for that size: the last k < lod_count with size below sizes[k]; force >= 0 forces one (clamped).
u32 pick_lod(const f32 sizes[kMaxLods], u32 lod_count, f32 size, i32 force);

// Automatic LODs. LOD k is built to about one triangle per four pixels at the size it takes over (a 1080p view),
// as long as its outline stays within about a pixel of the real shape; when that is not reachable it halves the
// LOD before it (the old rule); far LODs whose shape will not simplify further cluster their vertices instead
// (meshoptimizer's sloppy simplifier). Writes LOD k's indices to out[k - 1] (each with room for mesh.index_count)
// and their count to counts[k - 1]; returns how many LODs there are (1: the mesh gains nothing from them).
u32 lod_triangle_budget(f32 screen_size);
f32 lod_max_error(f32 screen_size);  // relative to the mesh's size: about a pixel when drawn at screen_size
u32 build_lods(const MeshData& mesh, const f32 sizes[kMaxLods], u32* const out[kMaxLods - 1], u32 counts[kMaxLods - 1], bool lock_border = false);

// An Instanced Static Mesh's copies (`count` of them, 3x4 rows each) reordered by the LOD each is drawn at seen
// from `eye`: out_rows holds LOD 0's copies, then LOD 1's... LOD k's from first[k] to first[k + 1]. Copies smaller
// on screen than `min_size` are left out (`culled` of them).
struct LodBuckets {
    u32 first[kMaxLods + 1] = {};
    u32 culled = 0;
};
void sort_by_lod(const f32* rows, u32 count, const f32 lo[3], const f32 hi[3], const f32 eye[3], f32 distance_scale, const f32 sizes[kMaxLods],
                 u32 lod_count, i32 force, f32 min_size, f32* out_rows, LodBuckets& out);

} // namespace lsd::render
