#pragma once
// Build Lighting (Unreal's Lightmass, as its Volumetric Lightmap): a grid of light probes over the
// level, baked on the CPU by tracing rays against the static meshes. Each probe stores the light
// arriving from every direction (the sky it can see, and light bounced once off the surfaces it
// sees, from the sun, local lights and the sky) as L1 spherical harmonics per colour. The mesh
// shaders use it instead of the flat Sky Light ambient: insides get dark, colours bleed onto
// neighbours, and moving objects pick it up too. Direct light stays real-time.
// Static meshes also get lightmaps (Unreal's lightmaps): xatlas charts every one of them into one
// atlas (World Settings > Lightmap Resolution); each texel stores the light reaching it from the sky
// and bounces (replacing the probes there) and how much of the sun reaches it (baked shadows).
// shortcut: one bounce, colours from the component and its Material's base colour (no textures).
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

#include <atomic>
#include <string>
#include <vector>

namespace lsd::editor {

// A lightmapped object: a copy of its mesh whose lightmap_uv points into the atlas, drawn instead of
// its own mesh. Found again by name, mesh and place (moved or swapped: not lightmapped until rebuilt).
struct LightmapObject {
    char name[64] = "";
    char mesh[40] = "";
    f32 position[3] = {};
    std::vector<rhi::MeshVertex> vertices;
    std::vector<u32> indices;
};

struct BakedLighting {
    f32 origin[3] = {};
    f32 cell[3] = {1.0f, 1.0f, 1.0f};
    u32 dims[3] = {};
    std::vector<f32> sh;  // per probe: r, g, b, each (L0, L1x, L1y, L1z)
    // Lightmaps: a square RGBA8 atlas (RGB: x / (1 + x) of the ambient light in units of `lightmap_scale`,
    // A: the sun's visibility), and the objects drawn with it.
    u32 lightmap_size = 0;
    f32 lightmap_scale = 1.0f;
    std::vector<u8> lightmap;
    // Directional lightmaps: per texel, where its baked light mostly comes from (RGB: the luminance-weighted mean
    // direction * 0.5 + 0.5, its length how one-sided the light is; normal maps shade by it). Empty: older bakes.
    std::vector<u8> lightmap_dir;
    // Shadow masks (Unreal's, for Stationary lights): per texel, how much of each of up to 4 such lights reaches it
    // (RGBA, one light per channel), and where those lights were (the run time finds its channel by place).
    u32 mask_count = 0;
    f32 mask_lights[4][3] = {};
    std::vector<u8> shadow_mask;
    std::vector<LightmapObject> objects;
    bool valid() const { return !sh.empty(); }
    bool has_lightmap() const { return lightmap_size && !objects.empty(); }
    u32 probe_count() const { return dims[0] * dims[1] * dims[2]; }
};

// What the bake traces against, gathered from the level on the main thread.
struct BakeScene {
    std::vector<Vec3> positions;  // 3 per triangle, world space
    std::vector<Vec3> albedo;     // 1 per triangle
    rhi::LightSetup lights;       // the level's lights, sky and ground colours
    u32 static_lights = 0;        // a bit per light: Static (its direct light is baked too); bit 16 + i: Stationary (its shadow mask)
    Vec3 lo, hi;                  // where probes go (a Lightmass Importance Volume, or the meshes' bounds)
    f32 spacing = 0.0f;           // metres between probes; 0: as fine as rhi::kMaxProbes allows
    // The objects to lightmap (World Settings > Lightmap Resolution; 0 = none) and their meshes.
    u32 lightmap_size = 0;
    struct LightmapSource {
        LightmapObject object;  // name, mesh, place, its mesh (object space, Box UV applied)
        Mat4 world;
    };
    std::vector<LightmapSource> lightmap_sources;
};

bool gather_bake_scene(const world::World& world, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, BakeScene& out);
// Runs on any thread (it uses all cores); `done` counts finished probes for a progress bar.
void bake_lighting(const BakeScene& scene, BakedLighting& out, std::atomic<u32>* done = nullptr, std::atomic<u32>* total = nullptr);

std::string baked_lighting_path(const char* level_path);  // Levels/Main.lsdlevel -> Levels/Main_BuiltData.lsdlight
bool save_baked_lighting(const char* path, const BakedLighting& lighting);
bool load_baked_lighting(const char* path, BakedLighting& lighting);
void apply_baked_lighting(const BakedLighting& lighting, rhi::LightSetup& setup);

// The lightmap object drawn for this one (same name and mesh, within 1 cm of where it was baked), or -1.
i32 find_lightmap_object(const BakedLighting& lighting, const char* name, const char* mesh, Vec3 position);

} // namespace lsd::editor
