#include "water.h"

#include "lsd/foundation/log.h"
#include "lsd/world/components.h"
#include "lsd/world/water.h"

#include <SDL3/SDL_timer.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

namespace {

// The water material (Material Graph code, HLSL spelling the OpenGL backend maps). Parameters:
// 0..3 the waves (wave vector xy, amplitude, angular speed; world::water_waves), 4 shallow colour +
// clarity, 5 deep colour + depth fade (m), 6 the camera + how far the wave normals reach.
constexpr char kWaterShader[] = R"(
Surface material_main(MaterialIn mi) {
    float3 slope = lsd_v3(0.0);
    for (int k = 0; k < 4; ++k) {
        float4 w = lsd_params[k];
        float c = w.z * cos(w.x * mi.wpos.x + w.y * mi.wpos.y - w.w * mi.time);
        slope.x += c * w.x;
        slope.y += c * w.y;
    }
    float far_away = saturate(length(mi.wpos - lsd_params[6].xyz) / max(lsd_params[6].w, 1.0));
    slope *= 1.0 - 0.8 * far_away;  // calmer toward the horizon: no sparkle from waves smaller than a pixel
    float3 n = normalize(float3(-slope.x, -slope.y, 1.0));
    float deep = 1.0 - exp(-max(mi.depth, 0.0) / max(lsd_params[5].w, 0.01));
    float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(abs(dot(n, mi.view))), 5.0);
    Surface s;
    s.base_color = lerp(lsd_params[4].rgb, lsd_params[5].rgb, deep);
    s.metallic = 0.0;
    s.roughness = 0.06;
    s.emissive = lsd_v3(0.0);
    s.normal = n;  // the mesh's UVs are world X / Y and its normals up: tangent space is world space
    s.ao = 1.0;
    s.opacity = lerp(lerp(1.0 - lsd_params[4].w, 1.0, deep), 1.0, fresnel);  // grazing views reflect, even in the shallows
    return s;
}
)";

constexpr u32 kParams = 7;
constexpr u32 kLakeCells = 96;   // most cells along a lake's side
constexpr u32 kOceanInner = 64;  // the ocean's fine square around the camera
constexpr u32 kRiverAcross = 6;

struct State {
    rhi::Device* device = nullptr;
    rhi::MaterialShaderHandle shader;
    std::vector<rhi::BufferHandle> live;                // this call's vertex buffers (released next call)
    std::unordered_map<u32, rhi::BufferHandle> grids;   // index buffers by (columns << 16 | rows)
    std::vector<rhi::MeshVertex> vertices;
    std::vector<f32> axis;
};
State g;  // shortcut: std containers and per-frame vertex buffers (editor-time code); a GPU-displaced grid when water gets big

// Two triangles a cell of a (cols + 1) x (rows + 1) grid of vertices, row by row; one buffer per size.
rhi::BufferHandle grid_indices(rhi::Device& device, u32 cols, u32 rows) {
    const u32 key = cols << 16 | rows;
    if (auto it = g.grids.find(key); it != g.grids.end()) return it->second;
    std::vector<u32> indices;
    indices.reserve(static_cast<usize>(cols) * rows * 6);
    for (u32 j = 0; j < rows; ++j) {
        for (u32 i = 0; i < cols; ++i) {
            const u32 a = j * (cols + 1) + i, b = a + 1, c = a + cols + 1, d = c + 1;
            indices.insert(indices.end(), {a, b, d, a, d, c});
        }
    }
    const rhi::BufferHandle buffer = device.create_buffer(rhi::BufferUsage::Index, indices.data(), indices.size() * sizeof(u32));
    g.grids[key] = buffer;
    return buffer;
}

void add_vertex(Vec3 p) { g.vertices.push_back({{p.x, p.y, p.z}, {0.0f, 0.0f, 1.0f}, {p.x, p.y}, {0.0f, 0.0f}}); }

// The ocean's grid lines along one axis around `centre`: even inside +-inner, then growing out to `reach`.
void ocean_axis(f32 centre, f32 inner, f32 reach) {
    g.axis.clear();
    f32 outer[24];
    u32 rings = 0;
    for (f32 d = inner * 1.6f; d < reach && rings < 23; d *= 1.6f) outer[rings++] = d;
    outer[rings++] = reach;
    for (u32 k = rings; k-- > 0;) g.axis.push_back(centre - outer[k]);
    for (u32 i = 0; i <= kOceanInner; ++i) g.axis.push_back(centre - inner + 2.0f * inner * static_cast<f32>(i) / kOceanInner);
    for (u32 k = 0; k < rings; ++k) g.axis.push_back(centre + outer[k]);
}

} // namespace

f32 water_time() { return static_cast<f32>(SDL_GetTicks()) * 0.001f; }

u32 water_draws(rhi::Device& device, const world::World& world, const Mat4& view_proj, Vec3 eye, f32 time, bool skip_hidden, WaterDraw* out, u32 capacity) {
    if (g.device != &device) {  // a new device: nothing of the old one's is ours any more
        g = State{};
        g.device = &device;
        g.shader = device.create_material_shader(kWaterShader);
        if (!g.shader.valid()) LSD_LOG_WARN("water", "The water material did not compile: water draws plain translucent");
    }
    for (rhi::BufferHandle b : g.live) device.destroy_buffer(b);
    g.live.clear();
    const reflect::TypeInfo& type = reflect::type_of<world::WaterBody>();
    u32 count = 0;
    for (u32 n = 0; n < world.count(type) && count < capacity; ++n) {
        const auto* w = static_cast<const world::WaterBody*>(world.at(type, n));
        const world::Entity e = world.owner(type, n);
        if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
        const Mat4 m = world.world_matrix(e);
        const f32 wavelength = std::fmax(w->wavelength, 0.5f);
        g.vertices.clear();
        u32 cols = 0, rows = 0;
        Vec3 centre{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        if (w->kind == 1) {  // ocean: a fine square around the camera, coarse rings out to the horizon
            const f32 inner = std::fmin(std::fmax(6.0f * wavelength, 20.0f), 300.0f);
            const f32 cell = 2.0f * inner / kOceanInner;
            const f32 cx = std::floor(eye.x / cell) * cell, cy = std::floor(eye.y / cell) * cell;  // snapped: the waves do not crawl as the camera moves
            ocean_axis(cx, inner, 3000.0f);
            const std::vector<f32> xs = g.axis;
            ocean_axis(cy, inner, 3000.0f);
            cols = rows = static_cast<u32>(xs.size()) - 1;
            for (f32 y : g.axis) {
                for (f32 x : xs) {
                    // Waves on the vertices fade out at the fine square's edge (the coarse rings stay level).
                    const f32 edge = std::fmax(std::fabs(x - cx), std::fabs(y - cy));
                    const f32 fade = std::fmin(std::fmax((inner - edge) / (0.25f * inner), 0.0f), 1.0f);
                    add_vertex({x, y, centre.z + fade * world::water_wave_height(*w, x, y, time)});
                }
            }
            centre = {cx, cy, centre.z};
        } else if (w->kind == 2) {  // river: a strip along the spline
            Vec3 points[world::kRiverSamples], left[world::kRiverSamples];
            const u32 samples = world::river_samples(world, e, points, left);
            if (samples < 2) continue;
            cols = kRiverAcross;
            rows = samples - 1;
            for (u32 s = 0; s < samples; ++s) {
                for (u32 i = 0; i <= kRiverAcross; ++i) {
                    const Vec3 p = points[s] + left[s] * (w->river_width * (static_cast<f32>(i) / kRiverAcross - 0.5f));
                    add_vertex({p.x, p.y, p.z + world::water_wave_height(*w, p.x, p.y, time)});
                }
            }
            centre = points[samples / 2];
        } else {  // lake: its rectangle in the object's space, cells a fifth of a wavelength
            const f32 cell = wavelength * 0.2f;
            cols = static_cast<u32>(std::fmin(std::fmax(std::ceil(w->size_x / cell), 1.0f), static_cast<f32>(kLakeCells)));
            rows = static_cast<u32>(std::fmin(std::fmax(std::ceil(w->size_y / cell), 1.0f), static_cast<f32>(kLakeCells)));
            for (u32 j = 0; j <= rows; ++j) {
                for (u32 i = 0; i <= cols; ++i) {
                    const Vec3 local{w->size_x * (static_cast<f32>(i) / static_cast<f32>(cols) - 0.5f), w->size_y * (static_cast<f32>(j) / static_cast<f32>(rows) - 0.5f), 0.0f};
                    const Vec3 p = transform_point(m, local);
                    add_vertex({p.x, p.y, p.z + world::water_wave_height(*w, p.x, p.y, time)});
                }
            }
        }
        const rhi::BufferHandle vertices = device.create_buffer(rhi::BufferUsage::Vertex, g.vertices.data(), g.vertices.size() * sizeof(rhi::MeshVertex));
        const rhi::BufferHandle indices = grid_indices(device, cols, rows);
        if (!vertices.valid() || !indices.valid()) continue;
        g.live.push_back(vertices);
        WaterDraw& d = out[count++];
        d.entity = e;
        d.centre = centre;
        rhi::MeshDraw& draw = d.draw;
        draw = rhi::MeshDraw{};
        draw.vertices = vertices;
        draw.indices = indices;
        draw.index_count = cols * rows * 6;
        std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);  // the vertices are in world space
        const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::memcpy(draw.normal_matrix, identity, sizeof identity);
        std::memcpy(draw.world, identity, sizeof identity);
        draw.camera_position[0] = eye.x;
        draw.camera_position[1] = eye.y;
        draw.camera_position[2] = eye.z;
        draw.blend = 1;
        draw.time = time;
        draw.roughness = 0.08f;
        draw.color[0] = draw.color[1] = draw.color[2] = draw.color[3] = 1.0f;
        if (!g.shader.valid()) {  // plain translucent water
            draw.color[0] = w->color.r, draw.color[1] = w->color.g, draw.color[2] = w->color.b;
            draw.color[3] = 1.0f - 0.5f * w->clarity;
            continue;
        }
        draw.material_shader = g.shader;
        draw.scene_depth = true;
        world::water_waves(*w, d.params);
        const f32 look[3][4] = {{w->color.r, w->color.g, w->color.b, w->clarity},
                                {w->deep_color.r, w->deep_color.g, w->deep_color.b, w->depth_fade},
                                {eye.x, eye.y, eye.z, 40.0f * wavelength}};
        std::memcpy(d.params[world::kWaterWaves], look, sizeof look);
        draw.material_params = d.params[0];
        draw.material_param_count = kParams;
    }
    return count;
}

} // namespace lsd::editor
