#pragma once
// Dynamic GI (World Settings > Dynamic Global Illumination; Unreal's Lumen in spirit, much simpler): a grid of
// light probes that follows the camera and is relit all the time on worker threads, so bounced light and sky
// occlusion follow moving lights, a moving sun and moving objects. Each sweep traces every probe against the level's
// static meshes (light_bake's BVH and probe tracer), lights what the rays hit with the lights (shadow rays) and with
// the last sweep's probes (a bounce per sweep: they add up to many), and blends the result into the last one. The
// lit shaders read it where they read Build Lighting's probes (LightSetup::probes): no shader of its own, so D3D12,
// Vulkan and OpenGL all have it.
// shortcut: CPU rays against static meshes only (no skeletal meshes, landscapes or instanced meshes; base colours
// without textures); a sweep takes tens of ms, so light lags a few frames. GPU probe tracing when that is too slow.
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lsd::asset { class AssetRegistry; }

namespace lsd::editor {

struct ProbeTracer;

// The grid's probes per axis (x fastest, z up): 500 of rhi::kMaxProbes, 10 x 10 cells across and 4 high.
constexpr u32 kGiDims[3] = {10, 10, 5};

// Pure maths (tests): the grid's first probe in whole cells for a camera at `eye`, the camera near its middle.
void gi_grid_origin(Vec3 eye, f32 cell, const u32 dims[3], i32 out[3]);
// Moves a grid's probes (12 floats each) from origin `from` to `to` (in cells): probes in both keep their values,
// the others are zero and get kept[i] = 0.
void gi_scroll(const i32 from[3], const i32 to[3], const u32 dims[3], const f32* in, f32* out, u8* kept);

class DynamicGi {
public:
    ~DynamicGi() { stop(); }
    // Console variables (main.cpp): r.DynamicGI, r.DynamicGI.Spacing, r.DynamicGI.Rays.
    static inline bool enabled = true;  // also read by world_lights (Static lights stay real-time while it runs)
    f32 spacing = 2.0f;  // metres between probes
    i32 rays = 64;       // per probe per sweep (8..256)
    // Once per frame for the main view: when the level asks for it (and r.DynamicGI is on) starts a sweep if none is
    // running (the scene gathered again only when a static mesh moved), and writes the newest probes into `lights`.
    void update(const world::World& world, const asset::AssetRegistry& registry, const char* data_dir, Vec3 eye, rhi::LightSetup& lights);
    // The probes of the last update into another view's lights (reflection captures, screenshots); nothing when off.
    void apply(rhi::LightSetup& lights) const;
    void stop();  // waits for a running sweep and forgets everything (off, a new level)
    bool on() const { return have_; }
    f64 sweep_ms() const { return sweep_ms_; }  // the last sweep, on the worker threads
    f64 main_ms() const { return main_ms_; }    // this frame's share on the main thread (gather, hand-over)

private:
    void start_sweep(const world::World& world, const asset::AssetRegistry& registry, const char* data_dir, Vec3 eye, const rhi::LightSetup& lights);

    struct MeshTris {
        std::vector<Vec3> positions;  // 3 per triangle, object space
        bool ok = false;
    };
    std::map<std::string, MeshTris> meshes_;  // by mesh key, loaded once
    std::map<std::string, Vec3> materials_;   // base colour by material GUID
    std::shared_ptr<ProbeTracer> tracer_;
    u64 scene_hash_ = 0;
    // The sweep running on the worker (one at a time); its inputs stay untouched until it is done.
    std::thread worker_;
    std::atomic<bool> done_{false};
    std::shared_ptr<ProbeTracer> pending_tracer_;
    std::vector<f32> history_, result_;  // in: the last probes moved to the sweep's origin; out: the new ones
    std::vector<u8> kept_;
    i32 sweep_origin_[3] = {};
    f32 sweep_cell_ = 2.0f;
    u32 sweep_count_ = 0;
    f64 sweep_start_ = 0.0;
    // What the shaders get: the newest finished sweep.
    std::vector<f32> sh_;
    i32 origin_[3] = {};
    f32 cell_ = 2.0f;
    bool have_ = false;
    f64 sweep_ms_ = 0.0, main_ms_ = 0.0;
};

} // namespace lsd::editor
