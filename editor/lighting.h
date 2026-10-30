#pragma once
// The level's light components as the renderer's light list: every Directional Light, then the
// Point / Spot Lights nearest the camera (up to rhi::kMaxLights in all), the Sky Light's ambient
// and the World Settings exposure. A level without lights keeps the default sun and sky.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

namespace lsd::editor {

rhi::LightSetup world_lights(const world::World& world, Vec3 eye, bool skip_hidden);

// A black body's colour at `kelvin` (linear RGB, 6500 K about white), for light temperature.
LinearColor temperature_color(f32 kelvin);

} // namespace lsd::editor
