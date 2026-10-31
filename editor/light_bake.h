#pragma once
// Build Lighting (Unreal's Lightmass, as its Volumetric Lightmap): a grid of light probes over the
// level, baked on the CPU by tracing rays against the static meshes. Each probe stores the light
// arriving from every direction (the sky it can see, and light bounced once off the surfaces it
// sees, from the sun, local lights and the sky) as L1 spherical harmonics per colour. The mesh
// shaders use it instead of the flat Sky Light ambient: insides get dark, colours bleed onto
// neighbours, and moving objects pick it up too. Direct light stays real-time.
// shortcut: probes only (no lightmap textures), one bounce, colours from the component and its
// Material's base colour (no textures); add lightmaps (xatlas UVs) when baked shadows need detail.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

#include <atomic>
#include <string>
#include <vector>

namespace lsd::editor {

struct BakedLighting {
    f32 origin[3] = {};
    f32 cell[3] = {1.0f, 1.0f, 1.0f};
    u32 dims[3] = {};
    std::vector<f32> sh;  // per probe: r, g, b, each (L0, L1x, L1y, L1z)
    bool valid() const { return !sh.empty(); }
    u32 probe_count() const { return dims[0] * dims[1] * dims[2]; }
};

// What the bake traces against, gathered from the level on the main thread.
struct BakeScene {
    std::vector<Vec3> positions;  // 3 per triangle, world space
    std::vector<Vec3> albedo;     // 1 per triangle
    rhi::LightSetup lights;       // the level's lights, sky and ground colours
    Vec3 lo, hi;                  // where probes go (a Lightmass Importance Volume, or the meshes' bounds)
    f32 spacing = 0.0f;           // metres between probes; 0: as fine as rhi::kMaxProbes allows
};

bool gather_bake_scene(const world::World& world, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, BakeScene& out);
// Runs on any thread (it uses all cores); `done` counts finished probes for a progress bar.
void bake_lighting(const BakeScene& scene, BakedLighting& out, std::atomic<u32>* done = nullptr, std::atomic<u32>* total = nullptr);

std::string baked_lighting_path(const char* level_path);  // Levels/Main.lsdlevel -> Levels/Main_BuiltData.lsdlight
bool save_baked_lighting(const char* path, const BakedLighting& lighting);
bool load_baked_lighting(const char* path, BakedLighting& lighting);
void apply_baked_lighting(const BakedLighting& lighting, rhi::LightSetup& setup);

} // namespace lsd::editor
