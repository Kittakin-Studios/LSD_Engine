#pragma once
// The level's light components as the renderer's light list: every Directional Light, then the
// Point / Spot Lights nearest the camera (up to rhi::kMaxLights in all), the Sky Light's ambient
// and the World Settings exposure. A level without lights keeps the default sun and sky.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

namespace lsd::editor {

struct BakedLighting;

// The camera the light list is built for: local lights nearest it, and sun shadow cascades fitted
// to its view (fov_y in radians).
struct LightView {
    Vec3 eye;
    Vec3 forward{1, 0, 0};
    f32 fov_y = 1.0f;
    f32 aspect = 1.0f;
};

// Shadow atlas size (both atlases): 4096 normally (sun: 2 x 2 cascades of 2048; local lights:
// 4 x 4 tiles of 1024), 2048 on cards with little video memory. Set once, before the atlases exist.
u32 shadow_atlas_size();
void set_shadow_atlas_size(u32 size);

// The level's baked light probes (Build Lighting), used by world_lights; nullptr: none.
void set_baked_lighting(const BakedLighting* lighting);
// Just the light list, sky and ground (what Build Lighting bakes with).
rhi::LightSetup level_lights(const world::World& world, const LightView& view);
// Lights, shadows, and the Height Fog / Post Process Volume the camera sees.
rhi::LightSetup world_lights(const world::World& world, const LightView& view, bool skip_hidden);
// Lit particles (Niagara's lit sprite material): what multiplies a particle's colour at `position` whose sprite faces
// `normal` (towards the camera): the sky / ground ambient plus each light's diffuse, wrapped so light from behind still
// shows through smoke, times exposure (the same scale the lit meshes write).
// shortcut: no shadows or baked probes; per vertex on the CPU, fine for hundreds of sprites, a GPU path for thousands
Vec3 particle_light(const rhi::LightSetup& lights, Vec3 position, Vec3 normal);
// The reflection captures in the atlas now (position xyz, radius w), in tile order; world_lights hands them on.
void set_reflection_probes(const f32 (*probes)[4], const f32 (*boxes)[4], u32 count);  // boxes: half size, fade (w 0 = sphere)
// Graphics quality (0 Low .. 3 Epic): Low has no shadows, Medium only the sun's and nearer, Epic
// casts them further.
void set_shadow_quality(u32 quality);
// Post Processing quality (0 Low .. 3 Epic): world_post trims its result to it (limit_post).
void set_post_quality(u32 quality);
// The look at `eye` (Unreal's volume blending): from the defaults, every Post Process Volume that reaches it, lowest
// priority first, each mixed in by its Blend Weight (fading out over its Blend Radius outside its box). Numbers and
// colours are blended; switches and the LUT come from a volume once it is at least half in. False when none reaches.
bool blend_post_volumes(const world::World& world, Vec3 eye, bool skip_hidden, world::PostProcessVolume& out);
// The post-process pass the camera gets from the level's Post Process Volumes (bloom, tonemap,
// grading, FXAA), blended by blend_post_volumes; the default volume's values where none applies. Always true for now.
// `chosen`: the blended look (nullptr when no volume applies: the defaults), for its Color Grading LUT; valid until the next call.
// out.contact_shadow comes back in world space (toward the first sun with a Contact Shadow Length).
bool world_post(const world::World& world, Vec3 eye, bool skip_hidden, rhi::PostDraw& out, const world::PostProcessVolume** chosen = nullptr);
// The level's Sky, if it has one (sun disc from the first Directional Light).
bool world_sky(const world::World& world, const Mat4& view_proj, Vec3 eye, bool skip_hidden, rhi::SkyDraw& out);

// A black body's colour at `kelvin` (linear RGB, 6500 K about white), for light temperature.
LinearColor temperature_color(f32 kelvin);

} // namespace lsd::editor
