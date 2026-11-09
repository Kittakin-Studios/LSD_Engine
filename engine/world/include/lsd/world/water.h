#pragma once
// Water Bodies (components.h WaterBody): their waves and where their water is, shared by the editor's
// renderer (editor/water.cpp: the surface mesh, and its shader sums the same waves per pixel) and by
// Play (swimming, floating), so what floats sits on the waves you see.
#include "lsd/foundation/math.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

namespace lsd::world {

constexpr u32 kWaterWaves = 4;

// The summed sine waves of a Water Body: per wave its wave vector (x, y: 2 pi / wavelength along its
// direction), amplitude (m) and angular speed (rad/s). The biggest goes along Wave Direction; three
// smaller, slower ones cross it at angles so the pattern never repeats in a straight line.
void water_waves(const WaterBody& w, f32 out[kWaterWaves][4]);
// Height of the waves above the rest surface at world (x, y) and `time` seconds; the slopes (dh/dx,
// dh/dy) when asked: the surface normal is normalize(-slope_x, -slope_y, 1).
f32 water_wave_height(const WaterBody& w, f32 x, f32 y, f32 time, f32* slope_x = nullptr, f32* slope_y = nullptr);

// River: points along the object's Spline in world space, about a metre apart (at most kRiverSamples),
// with the level unit vector to the left of each; the count (0 without a Spline of two points or more).
constexpr u32 kRiverSamples = 129;
u32 river_samples(const World& world, Entity e, Vec3 points[kRiverSamples], Vec3 left[kRiverSamples]);

// The water surface of Water Body `e` (waves included) above or below `p`: false when p is not over its
// water (outside the lake's rectangle, off the river); an ocean covers every (x, y).
bool water_surface(const World& world, Entity e, Vec3 p, f32 time, f32& height);
// Is `p` in any Water Body's water (at or below its surface, no deeper than its Depth)? `surface` gets
// that water's height there and `body` its object.
bool in_water(const World& world, Vec3 p, f32 time, f32* surface = nullptr, Entity* body = nullptr);

} // namespace lsd::world
