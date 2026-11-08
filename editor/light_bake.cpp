#include "light_bake.h"

#include "lighting.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"
#include "lsd/render/material.h"
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace lsd::editor {

namespace {

// Bounding volume hierarchy over the triangles: what makes tracing a few hundred thousand rays fast.
struct Bvh {
    struct Node {
        Vec3 lo, hi;
        u32 first, count;  // leaf: triangles [first, first + count); inner: count = 0, children at first, first + 1
    };
    std::vector<Node> nodes;
    std::vector<u32> order;  // triangle indices, leaves point into this
    const BakeScene* scene = nullptr;

    Vec3 vertex(u32 tri, u32 k) const { return scene->positions[tri * 3 + k]; }

    void build(const BakeScene& s) {
        scene = &s;
        const u32 count = static_cast<u32>(s.positions.size() / 3);
        order.resize(count);
        for (u32 i = 0; i < count; ++i) order[i] = i;
        nodes.clear();
        nodes.reserve(count * 2 + 1);
        nodes.push_back({});
        split(0, 0, count);
    }

    void split(u32 node, u32 first, u32 count) {
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, clo = lo, chi = hi;
        for (u32 i = first; i < first + count; ++i) {
            Vec3 centre{};
            for (u32 k = 0; k < 3; ++k) {
                const Vec3 v = vertex(order[i], k);
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
                centre = centre + v * (1.0f / 3.0f);
            }
            clo = {std::min(clo.x, centre.x), std::min(clo.y, centre.y), std::min(clo.z, centre.z)};
            chi = {std::max(chi.x, centre.x), std::max(chi.y, centre.y), std::max(chi.z, centre.z)};
        }
        nodes[node] = {lo, hi, first, count};
        if (count <= 4) return;
        const Vec3 extent = chi - clo;
        const int axis = extent.x > extent.y && extent.x > extent.z ? 0 : extent.y > extent.z ? 1 : 2;
        auto key = [&](u32 tri) {
            const Vec3 c = vertex(tri, 0) + vertex(tri, 1) + vertex(tri, 2);
            return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
        };
        const u32 mid = first + count / 2;
        std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count, [&](u32 a, u32 b) { return key(a) < key(b); });
        const u32 left = static_cast<u32>(nodes.size());
        nodes.push_back({});
        nodes.push_back({});
        nodes[node].first = left;
        nodes[node].count = 0;
        split(left, first, mid - first);
        split(left + 1, mid, first + count - mid);
    }

    static bool hit_box(const Node& n, Vec3 o, Vec3 inv, f32 t_max) {
        f32 t0 = 0.0f, t1 = t_max;
        const f32 lo[3] = {n.lo.x, n.lo.y, n.lo.z}, hi[3] = {n.hi.x, n.hi.y, n.hi.z}, oo[3] = {o.x, o.y, o.z}, ii[3] = {inv.x, inv.y, inv.z};
        for (int a = 0; a < 3; ++a) {
            f32 near_t = (lo[a] - oo[a]) * ii[a], far_t = (hi[a] - oo[a]) * ii[a];
            if (near_t > far_t) std::swap(near_t, far_t);
            t0 = near_t > t0 ? near_t : t0;
            t1 = far_t < t1 ? far_t : t1;
            if (t0 > t1) return false;
        }
        return true;
    }

    // Nearest hit along the ray (any hit if `any`): its triangle and distance.
    bool trace(Vec3 o, Vec3 d, f32 t_max, u32& tri_out, f32& t_out, bool any = false) const {
        if (nodes.empty() || order.empty()) return false;
        const Vec3 inv{1.0f / (std::fabs(d.x) > 1e-8f ? d.x : 1e-8f), 1.0f / (std::fabs(d.y) > 1e-8f ? d.y : 1e-8f), 1.0f / (std::fabs(d.z) > 1e-8f ? d.z : 1e-8f)};
        u32 stack[64];
        u32 top = 0;
        stack[top++] = 0;
        bool found = false;
        f32 best = t_max;
        while (top) {
            const Node& n = nodes[stack[--top]];
            if (!hit_box(n, o, inv, best)) continue;
            if (n.count == 0) {
                if (top + 2 <= 64) {
                    stack[top++] = n.first;
                    stack[top++] = n.first + 1;
                }
                continue;
            }
            for (u32 i = n.first; i < n.first + n.count; ++i) {  // Moller-Trumbore
                const u32 tri = order[i];
                const Vec3 a = vertex(tri, 0), e1 = vertex(tri, 1) - a, e2 = vertex(tri, 2) - a;
                const Vec3 p = cross(d, e2);
                const f32 det = dot(e1, p);
                if (std::fabs(det) < 1e-10f) continue;
                const f32 inv_det = 1.0f / det;
                const Vec3 s = o - a;
                const f32 u = dot(s, p) * inv_det;
                if (u < 0.0f || u > 1.0f) continue;
                const Vec3 q = cross(s, e1);
                const f32 v = dot(d, q) * inv_det;
                if (v < 0.0f || u + v > 1.0f) continue;
                const f32 t = dot(e2, q) * inv_det;
                if (t > 1e-4f && t < best) {
                    best = t;
                    tri_out = tri;
                    found = true;
                    if (any) return true;
                }
            }
        }
        t_out = best;
        return found;
    }
};

Vec3 sky_radiance(const rhi::LightSetup& l, Vec3 d) {
    const f32 t = d.z * 0.5f + 0.5f;
    return Vec3{l.ground[0], l.ground[1], l.ground[2]} * (1.0f - t) + Vec3{l.sky[0], l.sky[1], l.sky[2]} * t;
}

// Light leaving a surface point towards the probe: its colour times the light reaching it (one bounce).
Vec3 bounce(const Bvh& bvh, const BakeScene& s, Vec3 p, Vec3 n, Vec3 albedo) {
    Vec3 irradiance{};
    const rhi::LightSetup& l = s.lights;
    for (u32 i = 0; i < l.count; ++i) {
        const rhi::GpuLight& light = l.lights[i];
        Vec3 to;
        f32 atten = 1.0f, distance = 1e4f;
        if (light.type < 0.5f) {
            to = Vec3{-light.direction[0], -light.direction[1], -light.direction[2]};
        } else {
            const Vec3 d = Vec3{light.position[0], light.position[1], light.position[2]} - p;
            const f32 dist2 = std::max(dot(d, d), 1e-4f);
            distance = std::sqrt(dist2);
            to = d * (1.0f / distance);
            const f32 r = dist2 / (light.range * light.range);
            const f32 window = std::clamp(1.0f - r * r, 0.0f, 1.0f);
            atten = window * window / dist2;
            const f32 c = dot(to * -1.0f, Vec3{light.direction[0], light.direction[1], light.direction[2]});
            if (light.type > 1.5f && light.type < 2.5f) atten *= std::clamp((c - light.spot_outer_cos) / std::max(light.spot_inner_cos - light.spot_outer_cos, 1e-4f), 0.0f, 1.0f);
            if (light.type > 2.5f) atten *= std::max(c, 0.0f);  // rect / disc: only in front (shortcut: baked as a point at its centre)
        }
        const f32 ndl = dot(n, to);
        if (ndl <= 0.0f || atten <= 0.0f) continue;
        u32 tri;
        f32 t;
        if (bvh.trace(p + n * 0.01f, to, distance, tri, t, true)) continue;  // in shadow
        irradiance = irradiance + Vec3{light.color[0], light.color[1], light.color[2]} * (ndl * atten);
    }
    // shortcut: the sky at a hit point counts half (no second ray for its own occlusion)
    irradiance = irradiance + sky_radiance(l, n) * (3.14159265f * 0.5f);
    return Vec3{albedo.x * irradiance.x, albedo.y * irradiance.y, albedo.z * irradiance.z} * (1.0f / 3.14159265f);
}

// Returns false if the probe is inside geometry (most of its rays hit the back of a surface).
bool bake_probe(const Bvh& bvh, const BakeScene& s, Vec3 p, u32 seed, f32* sh) {
    u32 back_hits = 0;
    constexpr u32 kRays = 256;
    for (int i = 0; i < 12; ++i) sh[i] = 0.0f;
    const f32 twist = static_cast<f32>(seed % 997) * 0.618034f;  // a different spiral per probe: no shared banding
    for (u32 r = 0; r < kRays; ++r) {
        const f32 z = 1.0f - 2.0f * (static_cast<f32>(r) + 0.5f) / kRays;  // Fibonacci sphere
        const f32 radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 phi = static_cast<f32>(r) * 2.39996323f + twist;
        const Vec3 d{radius * std::cos(phi), radius * std::sin(phi), z};
        Vec3 radiance;
        u32 tri;
        f32 t;
        if (!bvh.trace(p, d, 1e4f, tri, t)) {
            radiance = sky_radiance(s.lights, d);
        } else {
            const Vec3 a = s.positions[tri * 3], b = s.positions[tri * 3 + 1], c = s.positions[tri * 3 + 2];
            Vec3 n = normalize(cross(b - a, c - a));
            if (dot(n, d) > 0.0f) {  // either side lights up (planes, thin walls)
                n = n * -1.0f;
                ++back_hits;
            }
            radiance = bounce(bvh, s, p + d * t, n, s.albedo[tri]);
        }
        const f32 basis[4] = {0.282095f, 0.488603f * d.x, 0.488603f * d.y, 0.488603f * d.z};
        const f32 rgb[3] = {radiance.x, radiance.y, radiance.z};
        for (int ch = 0; ch < 3; ++ch) {
            for (int k = 0; k < 4; ++k) sh[ch * 4 + k] += rgb[ch] * basis[k];
        }
    }
    const f32 weight = 4.0f * 3.14159265f / kRays;
    for (int i = 0; i < 12; ++i) sh[i] *= weight;
    return back_hits < kRays / 4;
}

bool load_mesh(const char* key, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, render::MeshData& out) {
    asset::Guid guid;
    i32 part = -1;
    if (!asset::parse_mesh_key(key, guid, part)) {
        if (std::strcmp(key, "Cube") == 0) out = render::make_cube(allocator);
        else if (std::strcmp(key, "Sphere") == 0) out = render::make_sphere(allocator);
        else if (std::strcmp(key, "Plane") == 0) out = render::make_plane(allocator);
        else return false;
        return true;
    }
    char compiled[300];
    return asset::compile_mesh_key(registry, key, data_dir, allocator, compiled, sizeof compiled) && asset::load_compiled_mesh(compiled, allocator, out);
}

} // namespace

namespace {

// A texel of the lightmap atlas: where it is on which surface.
struct LightmapTexel {
    Vec3 position, normal;
    bool set = false;
};

Vec3 normal_to_world(const Mat4& m, const f32* n) {  // cofactor of the 3x3: right under any scale
    const Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    return normalize(cross(c1, c2) * n[0] + cross(c2, c0) * n[1] + cross(c0, c1) * n[2]);
}

// Charts the sources into one atlas (xatlas, texel density from their world size); fills `objects`
// (meshes with lightmap_uv) and `texels` (each covered texel's surface point). False if it failed.
bool chart_lightmaps(const BakeScene& s, u32 size, std::vector<LightmapObject>& objects, std::vector<LightmapTexel>& texels) {
    xatlas::Atlas* atlas = xatlas::Create();
    std::vector<std::vector<Vec3>> world_positions(s.lightmap_sources.size()), world_normals(s.lightmap_sources.size());
    std::vector<u32> added;  // which sources made it into the atlas
    for (usize i = 0; i < s.lightmap_sources.size(); ++i) {
        const BakeScene::LightmapSource& src = s.lightmap_sources[i];
        auto& p = world_positions[i];
        auto& n = world_normals[i];
        for (const rhi::MeshVertex& v : src.object.vertices) {
            p.push_back(transform_point(src.world, Vec3{v.position[0], v.position[1], v.position[2]}));
            n.push_back(normal_to_world(src.world, v.normal));
        }
        xatlas::MeshDecl decl;
        decl.vertexPositionData = p.data();
        decl.vertexPositionStride = sizeof(Vec3);
        decl.vertexNormalData = n.data();
        decl.vertexNormalStride = sizeof(Vec3);
        decl.vertexCount = static_cast<u32>(p.size());
        decl.indexData = src.object.indices.data();
        decl.indexCount = static_cast<u32>(src.object.indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        if (xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success) added.push_back(static_cast<u32>(i));
        else LSD_LOG_WARN("lighting", "Lightmap: %s skipped (its mesh could not be charted)", src.object.name);
    }
    if (added.empty()) {
        xatlas::Destroy(atlas);
        return false;
    }
    xatlas::PackOptions pack;
    pack.resolution = size;
    pack.padding = 2;
    pack.bilinear = true;
    xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
    // The estimate can overshoot (more atlases, or one bigger than asked): coarser until it fits.
    for (int attempt = 0; attempt < 8 && (atlas->atlasCount > 1 || atlas->width > size || atlas->height > size); ++attempt) {
        const f32 over = static_cast<f32>(std::max(atlas->width, atlas->height)) / static_cast<f32>(size);
        pack.texelsPerUnit = atlas->texelsPerUnit * (over > 1.0f ? 0.97f / over : 0.85f);
        xatlas::PackCharts(atlas, pack);
    }
    const bool ok = atlas->atlasCount <= 1 && atlas->width && atlas->height && atlas->width <= size && atlas->height <= size;
    if (ok) {
        LSD_LOG_INFO("lighting", "Lightmap: %u objects, %u x %u texels used of %u, %.1f texels per metre", atlas->meshCount, atlas->width, atlas->height, size, atlas->texelsPerUnit);
        texels.assign(static_cast<usize>(size) * size, LightmapTexel{});
        for (u32 m = 0; m < atlas->meshCount; ++m) {
            const xatlas::Mesh& out = atlas->meshes[m];
            const u32 source = added[m];
            const BakeScene::LightmapSource& src = s.lightmap_sources[source];
            LightmapObject object;
            std::snprintf(object.name, sizeof object.name, "%s", src.object.name);
            std::snprintf(object.mesh, sizeof object.mesh, "%s", src.object.mesh);
            std::memcpy(object.position, src.object.position, sizeof object.position);
            object.vertices.resize(out.vertexCount);
            for (u32 v = 0; v < out.vertexCount; ++v) {
                object.vertices[v] = src.object.vertices[out.vertexArray[v].xref];
                const bool charted = out.vertexArray[v].atlasIndex == 0;
                object.vertices[v].lightmap_uv[0] = charted ? out.vertexArray[v].uv[0] / static_cast<f32>(size) : 0.0f;
                object.vertices[v].lightmap_uv[1] = charted ? out.vertexArray[v].uv[1] / static_cast<f32>(size) : 0.0f;
            }
            object.indices.assign(out.indexArray, out.indexArray + out.indexCount);
            // Rasterise each triangle in the atlas: the texel centres inside it get its surface point.
            for (u32 t = 0; t + 2 < out.indexCount; t += 3) {
                const u32 a = out.indexArray[t], b = out.indexArray[t + 1], c = out.indexArray[t + 2];
                const f32 ax = out.vertexArray[a].uv[0], ay = out.vertexArray[a].uv[1], bx = out.vertexArray[b].uv[0], by = out.vertexArray[b].uv[1];
                const f32 cx = out.vertexArray[c].uv[0], cy = out.vertexArray[c].uv[1];
                const f32 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                if (std::fabs(area) < 1e-8f || out.vertexArray[a].atlasIndex != 0) continue;
                const auto& wp = world_positions[source];
                const auto& wn = world_normals[source];
                const u32 xa = out.vertexArray[a].xref, xb = out.vertexArray[b].xref, xc = out.vertexArray[c].xref;
                const i32 x0 = std::max(0, static_cast<i32>(std::floor(std::min({ax, bx, cx})))), x1 = std::min(static_cast<i32>(size) - 1, static_cast<i32>(std::ceil(std::max({ax, bx, cx}))));
                const i32 y0 = std::max(0, static_cast<i32>(std::floor(std::min({ay, by, cy})))), y1 = std::min(static_cast<i32>(size) - 1, static_cast<i32>(std::ceil(std::max({ay, by, cy}))));
                // A triangle smaller than a texel covers no texel centre: it still owns the texels under
                // its corners and its middle (else detailed meshes come out black between their texels).
                auto claim = [&](f32 u, f32 v, f32 w0, f32 w1) {
                    const i32 x = std::clamp(static_cast<i32>(u), 0, static_cast<i32>(size) - 1), y = std::clamp(static_cast<i32>(v), 0, static_cast<i32>(size) - 1);
                    LightmapTexel& texel = texels[static_cast<usize>(y) * size + static_cast<usize>(x)];
                    if (texel.set) return;
                    texel.position = wp[xa] * w0 + wp[xb] * w1 + wp[xc] * (1.0f - w0 - w1);
                    texel.normal = normalize(wn[xa] * w0 + wn[xb] * w1 + wn[xc] * (1.0f - w0 - w1));
                    texel.set = true;
                };
                for (i32 y = y0; y <= y1; ++y) {
                    for (i32 x = x0; x <= x1; ++x) {
                        const f32 px = static_cast<f32>(x) + 0.5f, py = static_cast<f32>(y) + 0.5f;
                        const f32 w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                        const f32 w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                        const f32 w2 = 1.0f - w0 - w1;
                        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                        LightmapTexel& texel = texels[static_cast<usize>(y) * size + static_cast<usize>(x)];
                        if (texel.set) continue;
                        texel.position = wp[xa] * w0 + wp[xb] * w1 + wp[xc] * w2;
                        texel.normal = normalize(wn[xa] * w0 + wn[xb] * w1 + wn[xc] * w2);
                        texel.set = true;
                    }
                }
                claim((ax + bx + cx) / 3.0f, (ay + by + cy) / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
                claim(ax, ay, 0.98f, 0.01f);
                claim(bx, by, 0.01f, 0.98f);
                claim(cx, cy, 0.01f, 0.01f);
            }
            objects.push_back(std::move(object));
        }
    }
    xatlas::Destroy(atlas);
    return ok;
}

// The light a texel gets: from the sky and one bounce over its hemisphere (as the probes), and how
// much of the sun reaches it (a few rays over the sun's disc: soft edges).
void bake_texel(const Bvh& bvh, const BakeScene& s, const LightmapTexel& t, u32 seed, Vec3& ambient, f32& sun) {
    const Vec3 n = t.normal;
    const Vec3 helper = std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 tx = normalize(cross(helper, n)), ty = cross(n, tx);
    const Vec3 origin = t.position + n * 0.02f;
    constexpr u32 kRays = 64;
    const f32 twist = static_cast<f32>(seed % 1009) * 0.618034f;
    Vec3 sum{};
    for (u32 r = 0; r < kRays; ++r) {  // cosine-weighted (a Fibonacci disc lifted onto the hemisphere)
        const f32 u = (static_cast<f32>(r) + 0.5f) / kRays;
        const f32 radius = std::sqrt(u), phi = static_cast<f32>(r) * 2.39996323f + twist;
        const Vec3 d = tx * (radius * std::cos(phi)) + ty * (radius * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u));
        u32 tri;
        f32 dist;
        if (!bvh.trace(origin, d, 1e4f, tri, dist)) {
            sum = sum + sky_radiance(s.lights, d);
        } else {
            const Vec3 a = s.positions[tri * 3], b = s.positions[tri * 3 + 1], c = s.positions[tri * 3 + 2];
            Vec3 hn = normalize(cross(b - a, c - a));
            if (dot(hn, d) > 0.0f) hn = hn * -1.0f;
            sum = sum + bounce(bvh, s, origin + d * dist, hn, s.albedo[tri]);
        }
    }
    ambient = sum * (1.0f / kRays);  // irradiance / pi: what the shader's ambient term is in
    sun = 1.0f;
    for (u32 i = 0; i < s.lights.count; ++i) {
        const rhi::GpuLight& light = s.lights.lights[i];
        if (light.type >= 0.5f) continue;
        const Vec3 to = normalize(Vec3{-light.direction[0], -light.direction[1], -light.direction[2]});
        if (dot(to, n) <= 0.0f) break;  // faces away: no sun either way
        const Vec3 sx = normalize(cross(std::fabs(to.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, to)), sy = cross(to, sx);
        constexpr u32 kSunRays = 8;
        u32 lit = 0;
        for (u32 r = 0; r < kSunRays; ++r) {
            const f32 phi = static_cast<f32>(r) * 2.39996323f + twist, spread = 0.0087f * std::sqrt((static_cast<f32>(r) + 0.5f) / kSunRays);  // half a degree
            const Vec3 d = normalize(to + sx * (spread * std::cos(phi)) + sy * (spread * std::sin(phi)));
            u32 tri;
            f32 dist;
            if (!bvh.trace(origin, d, 1e4f, tri, dist, true)) ++lit;
        }
        sun = static_cast<f32>(lit) / kSunRays;
        break;  // the first directional light: the one the shader shadows
    }
}

void bake_lightmaps(const Bvh& bvh, const BakeScene& s, BakedLighting& out, std::atomic<u32>* done, std::atomic<u32>* total) {
    if (!s.lightmap_size || s.lightmap_sources.empty()) return;
    const u32 size = s.lightmap_size;
    std::vector<LightmapTexel> texels;
    if (!chart_lightmaps(s, size, out.objects, texels)) {
        LSD_LOG_WARN("lighting", "Lightmap: the objects could not be charted; light probes only");
        out.objects.clear();
        return;
    }
    std::vector<u32> todo;
    for (u32 i = 0; i < texels.size(); ++i) {
        if (texels[i].set) todo.push_back(i);
    }
    if (total) *total += static_cast<u32>(todo.size());
    std::vector<Vec3> ambient(texels.size());
    std::vector<f32> sun(texels.size(), 1.0f);
    std::atomic<u32> next{0};
    auto worker = [&] {
        for (u32 k = next++; k < todo.size(); k = next++) {
            bake_texel(bvh, s, texels[todo[k]], todo[k], ambient[todo[k]], sun[todo[k]]);
            if (done) ++*done;
        }
    };
    const u32 threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    // Grow each chart by a few texels (Unreal's dilation): bilinear filtering at chart edges reads
    // texels outside the triangles, which would otherwise be black.
    std::vector<u8> set(texels.size());
    for (usize i = 0; i < texels.size(); ++i) set[i] = texels[i].set ? 1 : 0;
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<u8> next_set = set;
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                const usize i = static_cast<usize>(y) * size + x;
                if (set[i]) continue;
                Vec3 a{};
                f32 b = 0.0f;
                u32 count = 0;
                for (int o = 0; o < 4; ++o) {
                    const i32 nx = static_cast<i32>(x) + (o == 0 ? 1 : o == 1 ? -1 : 0), ny = static_cast<i32>(y) + (o == 2 ? 1 : o == 3 ? -1 : 0);
                    if (nx < 0 || ny < 0 || nx >= static_cast<i32>(size) || ny >= static_cast<i32>(size)) continue;
                    const usize j = static_cast<usize>(ny) * size + static_cast<usize>(nx);
                    if (!set[j]) continue;
                    a = a + ambient[j];
                    b += sun[j];
                    ++count;
                }
                if (!count) continue;
                ambient[i] = a * (1.0f / static_cast<f32>(count));
                sun[i] = b / static_cast<f32>(count);
                next_set[i] = 1;
            }
        }
        set.swap(next_set);
    }
    // Stored as x / (1 + x) of the light in units of its average: precise in the dark, never clipped.
    f64 sum = 0.0;
    for (u32 i : todo) sum += std::max({ambient[i].x, ambient[i].y, ambient[i].z});
    out.lightmap_scale = std::max(static_cast<f32>(sum / static_cast<f64>(std::max<usize>(todo.size(), 1))), 1e-6f);
    out.lightmap_size = size;
    out.lightmap.assign(static_cast<usize>(size) * size * 4, 0);
    for (usize i = 0; i < texels.size(); ++i) {
        u8* p = &out.lightmap[i * 4];
        const f32 c[3] = {ambient[i].x / out.lightmap_scale, ambient[i].y / out.lightmap_scale, ambient[i].z / out.lightmap_scale};
        for (int k = 0; k < 3; ++k) p[k] = static_cast<u8>(std::lround(std::clamp(c[k] / (1.0f + c[k]), 0.0f, 1.0f) * 255.0f));
        p[3] = static_cast<u8>(std::lround(std::clamp(set[i] ? sun[i] : 1.0f, 0.0f, 1.0f) * 255.0f));
    }
}

} // namespace

i32 find_lightmap_object(const BakedLighting& l, const char* name, const char* mesh, Vec3 position) {
    for (usize i = 0; i < l.objects.size(); ++i) {
        const LightmapObject& o = l.objects[i];
        if (std::strcmp(o.name, name) != 0 || std::strcmp(o.mesh, mesh) != 0) continue;
        const Vec3 d = position - Vec3{o.position[0], o.position[1], o.position[2]};
        if (dot(d, d) < 1e-4f) return static_cast<i32>(i);
    }
    return -1;
}

bool gather_bake_scene(const world::World& w, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, BakeScene& out) {
    out = BakeScene{};
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    const reflect::TypeInfo& mesh_type = reflect::type_of<world::StaticMesh>();
    for (u32 i = 0; i < w.count(mesh_type); ++i) {
        const world::Entity e = w.owner(mesh_type, i);
        if (w.hidden_in_hierarchy(e)) continue;
        const auto* m = static_cast<const world::StaticMesh*>(w.at(mesh_type, i));
        render::MeshData data;
        if (!load_mesh(m->mesh, registry, data_dir, allocator, data)) continue;
        Vec3 albedo{m->color.r, m->color.g, m->color.b};
        asset::Guid material_guid;
        if (m->material[0] && asset::parse_guid(m->material, material_guid)) {
            if (const asset::AssetRecord* r = registry.find(material_guid)) {
                char path[300];
                registry.source_path(*r, path, sizeof path);
                render::Material material;
                if (asset::load_data_asset(path, material)) albedo = Vec3{albedo.x * material.base_color.r, albedo.y * material.base_color.g, albedo.z * material.base_color.b};
            }
        }
        const Mat4 world_matrix = w.world_matrix(e);
        // Lightmapped: static (not a physics body, mover, vehicle or transport) and asked for.
        const bool moves = w.get<world::RigidBody>(e) || w.get<world::Mover>(e) || w.get<world::Vehicle>(e) || w.get<world::Transport>(e);
        if (w.settings().lightmap_resolution > 0 && m->static_lighting && !moves && data.index_count >= 3) {
            BakeScene::LightmapSource src;
            std::snprintf(src.object.name, sizeof src.object.name, "%s", w.name(e));
            std::snprintf(src.object.mesh, sizeof src.object.mesh, "%s", m->mesh);
            src.object.position[0] = world_matrix.at(0, 3);
            src.object.position[1] = world_matrix.at(1, 3);
            src.object.position[2] = world_matrix.at(2, 3);
            src.world = world_matrix;
            if (m->box_uv) {  // the same UVs the cache gives it (main.cpp: get_box_uv)
                render::box_project_uv(data, length(Vec3{world_matrix.at(0, 0), world_matrix.at(1, 0), world_matrix.at(2, 0)}),
                                       length(Vec3{world_matrix.at(0, 1), world_matrix.at(1, 1), world_matrix.at(2, 1)}),
                                       length(Vec3{world_matrix.at(0, 2), world_matrix.at(1, 2), world_matrix.at(2, 2)}));
            }
            src.object.vertices.assign(data.vertices, data.vertices + data.vertex_count);
            src.object.indices.assign(data.indices, data.indices + data.index_count);
            out.lightmap_sources.push_back(std::move(src));
        }
        for (u32 k = 0; k + 2 < data.index_count; k += 3) {
            for (u32 v = 0; v < 3; ++v) {
                const f32* p = data.vertices[data.indices[k + v]].position;
                const Vec3 wp = transform_point(world_matrix, Vec3{p[0], p[1], p[2]});
                out.positions.push_back(wp);
                lo = {std::min(lo.x, wp.x), std::min(lo.y, wp.y), std::min(lo.z, wp.z)};
                hi = {std::max(hi.x, wp.x), std::max(hi.y, wp.y), std::max(hi.z, wp.z)};
            }
            out.albedo.push_back(albedo);
        }
        render::free_mesh(allocator, data);
    }
    if (out.positions.empty()) return false;
    // A Lightmass Importance Volume (a box, like the Post Process Volume's) limits where probes go.
    const reflect::TypeInfo& volume_type = reflect::type_of<world::LightmassImportanceVolume>();
    if (w.count(volume_type) > 0) {
        const world::Entity e = w.owner(volume_type, 0);
        const Mat4 m = w.world_matrix(e);
        Vec3 vlo{1e30f, 1e30f, 1e30f}, vhi{-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const Vec3 p = transform_point(m, Vec3{(c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 1.0f : 0.0f});
            vlo = {std::min(vlo.x, p.x), std::min(vlo.y, p.y), std::min(vlo.z, p.z)};
            vhi = {std::max(vhi.x, p.x), std::max(vhi.y, p.y), std::max(vhi.z, p.z)};
        }
        lo = vlo;
        hi = vhi;
        out.spacing = static_cast<const world::LightmassImportanceVolume*>(w.at(volume_type, 0))->probe_spacing;
    } else {
        hi.z = std::min(hi.z, lo.z + 30.0f);  // shortcut: without a volume, probes stop 30 m above the lowest mesh
    }
    out.lo = lo;
    out.hi = hi;
    static const u32 kSizes[] = {0, 256, 512, 1024, 2048, 4096};
    out.lightmap_size = kSizes[std::min<u32>(w.settings().lightmap_resolution, 5)];
    if (!out.lightmap_size) out.lightmap_sources.clear();
    LightView view;
    view.eye = (lo + hi) * 0.5f;
    out.lights = level_lights(w, view);
    return true;
}

void bake_lighting(const BakeScene& s, BakedLighting& out, std::atomic<u32>* done, std::atomic<u32>* total) {
    out = BakedLighting{};
    const Vec3 size = s.hi - s.lo;
    // Probe spacing: as asked, or the finest the probe budget allows; at least 2 probes per axis.
    f32 cell = s.spacing > 0.0f ? s.spacing : std::max(0.5f, std::cbrt(std::max(size.x * size.y * size.z, 1e-3f) / rhi::kMaxProbes));
    u32 dims[3];
    for (;;) {
        dims[0] = std::max(2u, static_cast<u32>(std::ceil(size.x / cell)) + 1);
        dims[1] = std::max(2u, static_cast<u32>(std::ceil(size.y / cell)) + 1);
        dims[2] = std::max(2u, static_cast<u32>(std::ceil(size.z / cell)) + 1);
        if (dims[0] * dims[1] * dims[2] <= rhi::kMaxProbes) break;
        cell *= 1.1f;
    }
    for (int a = 0; a < 3; ++a) {
        out.dims[a] = dims[a];
        const f32 extent = a == 0 ? size.x : a == 1 ? size.y : size.z;
        out.cell[a] = std::max(extent / static_cast<f32>(dims[a] - 1), 0.01f);
    }
    out.origin[0] = s.lo.x;
    out.origin[1] = s.lo.y;
    out.origin[2] = s.lo.z;
    const u32 count = out.probe_count();
    if (total) *total = count;
    std::vector<f32> sh(static_cast<usize>(count) * 12);
    std::vector<u8> valid(count, 0);
    Bvh bvh;
    bvh.build(s);
    std::atomic<u32> next{0};
    auto worker = [&] {
        for (u32 i = next++; i < count; i = next++) {
            const u32 x = i % dims[0], y = (i / dims[0]) % dims[1], z = i / (dims[0] * dims[1]);
            const Vec3 p{out.origin[0] + out.cell[0] * x, out.origin[1] + out.cell[1] * y, out.origin[2] + out.cell[2] * z};
            valid[i] = bake_probe(bvh, s, p, i, &sh[static_cast<usize>(i) * 12]) ? 1 : 0;
            if (done) ++*done;
        }
    };
    const u32 threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    bake_lightmaps(bvh, s, out, done, total);
    // Probes inside walls would read black and darken the rooms beside them: fill each from its
    // valid neighbours (Unreal's invalid-sample fix), spreading a few cells into solid regions.
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<u8> next_valid = valid;
        for (u32 i = 0; i < count; ++i) {
            if (valid[i]) continue;
            const i32 x = static_cast<i32>(i % dims[0]), y = static_cast<i32>((i / dims[0]) % dims[1]), z = static_cast<i32>(i / (dims[0] * dims[1]));
            f32 sum[12] = {};
            u32 n = 0;
            const i32 offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (const auto& o : offsets) {
                const i32 nx = x + o[0], ny = y + o[1], nz = z + o[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= static_cast<i32>(dims[0]) || ny >= static_cast<i32>(dims[1]) || nz >= static_cast<i32>(dims[2])) continue;
                const u32 j = static_cast<u32>(nx) + static_cast<u32>(ny) * dims[0] + static_cast<u32>(nz) * dims[0] * dims[1];
                if (!valid[j]) continue;
                for (int k = 0; k < 12; ++k) sum[k] += sh[static_cast<usize>(j) * 12 + static_cast<usize>(k)];
                ++n;
            }
            if (!n) continue;
            for (int k = 0; k < 12; ++k) sh[static_cast<usize>(i) * 12 + static_cast<usize>(k)] = sum[k] / static_cast<f32>(n);
            next_valid[i] = 1;
        }
        valid.swap(next_valid);
    }
    out.sh = std::move(sh);
}

std::string baked_lighting_path(const char* level_path) {
    std::filesystem::path p(level_path);
    return (p.parent_path() / (p.stem().string() + "_BuiltData.lsdlight")).generic_string();
}

namespace {
constexpr char kMagic[8] = {'L', 'S', 'D', 'L', 'I', 'G', 'H', '1'};   // probes only
constexpr char kMagic2[8] = {'L', 'S', 'D', 'L', 'I', 'G', 'H', '2'};  // probes, then the lightmap and its objects
}

bool save_baked_lighting(const char* path, const BakedLighting& l) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(kMagic2, sizeof kMagic2, 1, f) == 1 && std::fwrite(l.origin, sizeof l.origin, 1, f) == 1 && std::fwrite(l.cell, sizeof l.cell, 1, f) == 1 &&
              std::fwrite(l.dims, sizeof l.dims, 1, f) == 1 && std::fwrite(l.sh.data(), sizeof(f32), l.sh.size(), f) == l.sh.size();
    const u32 size = l.has_lightmap() ? l.lightmap_size : 0, objects = size ? static_cast<u32>(l.objects.size()) : 0;
    ok = ok && std::fwrite(&size, sizeof size, 1, f) == 1 && std::fwrite(&l.lightmap_scale, sizeof l.lightmap_scale, 1, f) == 1 && std::fwrite(&objects, sizeof objects, 1, f) == 1;
    if (size) ok = ok && std::fwrite(l.lightmap.data(), 1, l.lightmap.size(), f) == l.lightmap.size();
    for (u32 i = 0; ok && i < objects; ++i) {
        const LightmapObject& o = l.objects[i];
        const u32 counts[2] = {static_cast<u32>(o.vertices.size()), static_cast<u32>(o.indices.size())};
        ok = std::fwrite(o.name, sizeof o.name, 1, f) == 1 && std::fwrite(o.mesh, sizeof o.mesh, 1, f) == 1 && std::fwrite(o.position, sizeof o.position, 1, f) == 1 &&
             std::fwrite(counts, sizeof counts, 1, f) == 1 && std::fwrite(o.vertices.data(), sizeof(rhi::MeshVertex), counts[0], f) == counts[0] &&
             std::fwrite(o.indices.data(), sizeof(u32), counts[1], f) == counts[1];
    }
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

bool load_baked_lighting(const char* path, BakedLighting& l) {
    l = BakedLighting{};
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    bool ok = std::fread(magic, sizeof magic, 1, f) == 1;
    const bool v2 = ok && std::memcmp(magic, kMagic2, sizeof magic) == 0;
    ok = ok && (v2 || std::memcmp(magic, kMagic, sizeof magic) == 0) && std::fread(l.origin, sizeof l.origin, 1, f) == 1 &&
         std::fread(l.cell, sizeof l.cell, 1, f) == 1 && std::fread(l.dims, sizeof l.dims, 1, f) == 1;
    const u64 count = ok ? static_cast<u64>(l.dims[0]) * l.dims[1] * l.dims[2] : 0;
    ok = ok && count >= 8 && count <= rhi::kMaxProbes;
    if (ok) {
        l.sh.resize(count * 12);
        ok = std::fread(l.sh.data(), sizeof(f32), l.sh.size(), f) == l.sh.size();
    }
    u32 size = 0, objects = 0;
    if (ok && v2) ok = std::fread(&size, sizeof size, 1, f) == 1 && std::fread(&l.lightmap_scale, sizeof l.lightmap_scale, 1, f) == 1 && std::fread(&objects, sizeof objects, 1, f) == 1 &&
                      size <= 4096 && objects <= 100000 && std::isfinite(l.lightmap_scale) && l.lightmap_scale > 0.0f;
    if (ok && size) {
        l.lightmap_size = size;
        l.lightmap.resize(static_cast<usize>(size) * size * 4);
        ok = std::fread(l.lightmap.data(), 1, l.lightmap.size(), f) == l.lightmap.size();
    }
    for (u32 i = 0; ok && i < objects; ++i) {  // never trust file data: counts capped, indices checked
        LightmapObject o;
        u32 counts[2] = {};
        ok = std::fread(o.name, sizeof o.name, 1, f) == 1 && std::fread(o.mesh, sizeof o.mesh, 1, f) == 1 && std::fread(o.position, sizeof o.position, 1, f) == 1 &&
             std::fread(counts, sizeof counts, 1, f) == 1 && counts[0] > 0 && counts[0] <= (1u << 24) && counts[1] >= 3 && counts[1] <= (1u << 26);
        if (!ok) break;
        o.name[sizeof o.name - 1] = '\0';
        o.mesh[sizeof o.mesh - 1] = '\0';
        o.vertices.resize(counts[0]);
        o.indices.resize(counts[1]);
        ok = std::fread(o.vertices.data(), sizeof(rhi::MeshVertex), counts[0], f) == counts[0] && std::fread(o.indices.data(), sizeof(u32), counts[1], f) == counts[1];
        for (u32 k = 0; ok && k < counts[1]; ++k) ok = o.indices[k] < counts[0];
        if (ok) l.objects.push_back(std::move(o));
    }
    std::fclose(f);
    if (!ok) l = BakedLighting{};
    return ok;
}

void apply_baked_lighting(const BakedLighting& l, rhi::LightSetup& setup) {
    if (!l.valid()) return;
    for (int a = 0; a < 3; ++a) {
        setup.probe_origin[a] = l.origin[a];
        setup.probe_cell[a] = l.cell[a];
        setup.probe_dims[a] = l.dims[a];
    }
    setup.probe_origin[3] = 1.0f;
    std::memcpy(setup.probes, l.sh.data(), l.sh.size() * sizeof(f32));
    if (l.has_lightmap()) {  // main.cpp uploads the atlas (set_lightmap_atlas) with the lighting
        setup.lightmap_info[0] = l.lightmap_scale;
        setup.lightmap_info[1] = 1.0f;
        setup.lightmap_info[2] = static_cast<f32>(l.lightmap_size);
    }
}

} // namespace lsd::editor
