#pragma once
// Spline math: a curve through every point (like Unreal's curve-type spline points). Each point's
// tangent is automatic (Catmull-Rom: half the difference of its neighbours) unless `tangents` gives
// one (a non-zero vector; tangent handles). Parameter t runs 0..segments, one unit per segment;
// distances are in metres along the curve, measured on a sampled table.
#include "lsd/foundation/math.h"
#include "lsd/world/world.h"

namespace lsd::world {

u32 spline_segments(u32 point_count, bool closed);
Vec3 spline_point(const Vec3* points, u32 count, bool closed, f32 t, const Vec3* tangents = nullptr);
Vec3 spline_tangent(const Vec3* points, u32 count, bool closed, f32 t, const Vec3* tangents = nullptr);  // not normalised
f32 spline_length(const Vec3* points, u32 count, bool closed, const Vec3* tangents = nullptr);
// Position at a distance along the curve (clamped to 0..length); `tangent` gets the direction there.
Vec3 spline_at_distance(const Vec3* points, u32 count, bool closed, f32 distance, Vec3* tangent = nullptr, const Vec3* tangents = nullptr);

// An object's Spline in world space: up to 32 points and their tangents (zero = automatic), into the
// arrays given; returns the point count (0 without a Spline) and sets `closed`.
u32 spline_world_curve(const World& world, Entity e, Vec3 points[32], Vec3 tangents[32], bool& closed);

// Spline Meshes: refills each such object's Instanced Static Mesh (added if missing) with copies along
// its Spline, in the object's own space. Cheap when nothing changed; the editor calls it every frame.
void update_spline_meshes(World& world);

} // namespace lsd::world