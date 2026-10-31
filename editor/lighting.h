#pragma once
// The level's light components as the renderer's light list: every Directional Light, then the
// Point / Spot Lights nearest the camera (up to rhi::kMaxLights in all), the Sky Light's ambient
// and the World Settings exposure. A level without lights keeps the default sun and sky.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

namespace lsd::editor {

// The camera the light list is built for: local lights nearest it, and sun shadow cascades fitted
// to its view (fov_y in radians).
struct LightView {
    Vec3 eye;
    Vec3 forward{1, 0, 0};
    f32 fov_y = 1.0f;
    f32 aspect = 1.0f;
};

constexpr u32 kShadowAtlasSize = 4096;  // sun: 2 x 2 cascades of 2048; local lights: 4 x 4 tiles of 1024

// Lights, shadows, and the Height Fog / Post Process Volume the camera sees.
rhi::LightSetup world_lights(const world::World& world, const LightView& view, bool skip_hidden);
// Graphics quality (0 Low .. 3 Epic): Low has no shadows, Medium only the sun's and nearer, Epic
// casts them further.
void set_shadow_quality(u32 quality);
// The post-process pass the camera gets from the level's Post Process Volume (bloom, tonemap,
// grading, FXAA); false if no volume applies (then the forward shaders grade directly).
bool world_post(const world::World& world, Vec3 eye, bool skip_hidden, rhi::PostDraw& out);
// The level's Sky, if it has one (sun disc from the first Directional Light).
bool world_sky(const world::World& world, const Mat4& view_proj, Vec3 eye, bool skip_hidden, rhi::SkyDraw& out);

// A black body's colour at `kelvin` (linear RGB, 6500 K about white), for light temperature.
LinearColor temperature_color(f32 kelvin);

} // namespace lsd::editor
