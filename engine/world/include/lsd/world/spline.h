#pragma once
// Spline math: a Catmull-Rom curve that passes through every point (like Unreal's curve-type
// spline points). Parameter t runs 0..segments, one unit per segment; distances are in metres
// along the curve, measured on a sampled table.
#include "lsd/foundation/math.h"

namespace lsd::world {

u32 spline_segments(u32 point_count, bool closed);
Vec3 spline_point(const Vec3* points, u32 count, bool closed, f32 t);
Vec3 spline_tangent(const Vec3* points, u32 count, bool closed, f32 t);  // not normalised
f32 spline_length(const Vec3* points, u32 count, bool closed);
// Position at a distance along the curve (clamped to 0..length); `tangent` gets the direction there.
Vec3 spline_at_distance(const Vec3* points, u32 count, bool closed, f32 distance, Vec3* tangent = nullptr);

} // namespace lsd::world
