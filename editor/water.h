#pragma once
// Water Bodies drawn (Unreal's Water plugin, the smallest version): each one's surface as a grid in
// world space, its waves moved on the CPU every frame (the same sum lsd/world/water.h gives gameplay),
// shaded by a built-in material (water.cpp): per-pixel wave normals, sky / reflection-capture
// reflections with fresnel, darker and more opaque where the depth behind it is greater.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

namespace lsd::editor {

// One Water Body's surface for this view, to draw with the translucent meshes (far to near).
struct WaterDraw {
    rhi::MeshDraw draw;
    Vec3 centre;        // for sorting
    world::Entity entity;
    f32 params[7][4];   // its material's parameters (draw.material_params points here)
};

// Builds the draws of every Water Body in `world` (hidden ones skipped when `skip_hidden`) for a view
// from `eye`; `time` in seconds drives the waves (the clock Play's swimming and floating use). The
// vertex buffers made last call are released (draw_world runs once per view). Returns the count.
u32 water_draws(rhi::Device& device, const world::World& world, const Mat4& view_proj, Vec3 eye, f32 time, bool skip_hidden, WaterDraw* out, u32 capacity);

// The clock both the drawn waves and Play's water use (seconds since the program started).
f32 water_time();

} // namespace lsd::editor
