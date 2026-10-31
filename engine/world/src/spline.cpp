#include "lsd/world/spline.h"
#include "lsd/world/components.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace lsd::world {

namespace {

constexpr u32 kSamplesPerSegment = 16;

u32 wrap(i64 i, u32 n, bool closed) {
    if (closed) return static_cast<u32>(((i % static_cast<i64>(n)) + static_cast<i64>(n)) % static_cast<i64>(n));
    return static_cast<u32>(i < 0 ? 0 : i >= static_cast<i64>(n) ? static_cast<i64>(n) - 1 : i);
}

// The tangent at point i: its own when set (non-zero), else Catmull-Rom's (neighbours' difference / 2;
// ends repeat themselves when open, as before).
Vec3 point_tangent(const Vec3* p, const Vec3* tangents, u32 n, bool closed, u32 i) {
    if (tangents && (tangents[i].x != 0.0f || tangents[i].y != 0.0f || tangents[i].z != 0.0f)) return tangents[i];
    return (p[wrap(static_cast<i64>(i) + 1, n, closed)] - p[wrap(static_cast<i64>(i) - 1, n, closed)]) * 0.5f;
}

void locate(u32 n, bool closed, f32 t, u32& segment, f32& u) {
    const u32 segments = spline_segments(n, closed);
    t = t < 0.0f ? 0.0f : t > static_cast<f32>(segments) ? static_cast<f32>(segments) : t;
    segment = static_cast<u32>(t);
    if (segment >= segments) segment = segments - 1;
    u = t - static_cast<f32>(segment);
}

} // namespace

u32 spline_segments(u32 n, bool closed) { return n < 2 ? 0 : closed ? n : n - 1; }

// Cubic Hermite between point s and the next, with their tangents (uniform Catmull-Rom when automatic).
Vec3 spline_point(const Vec3* p, u32 n, bool closed, f32 t, const Vec3* tangents) {
    if (n == 0) return {};
    if (n == 1) return p[0];
    u32 s;
    f32 u;
    locate(n, closed, t, s, u);
    const u32 e = wrap(static_cast<i64>(s) + 1, n, closed);
    const Vec3 m0 = point_tangent(p, tangents, n, closed, s), m1 = point_tangent(p, tangents, n, closed, e);
    const f32 u2 = u * u, u3 = u2 * u;
    return p[s] * (2.0f * u3 - 3.0f * u2 + 1.0f) + m0 * (u3 - 2.0f * u2 + u) + p[e] * (3.0f * u2 - 2.0f * u3) + m1 * (u3 - u2);
}

Vec3 spline_tangent(const Vec3* p, u32 n, bool closed, f32 t, const Vec3* tangents) {
    if (n < 2) return {1, 0, 0};
    u32 s;
    f32 u;
    locate(n, closed, t, s, u);
    const u32 e = wrap(static_cast<i64>(s) + 1, n, closed);
    const Vec3 m0 = point_tangent(p, tangents, n, closed, s), m1 = point_tangent(p, tangents, n, closed, e);
    const f32 u2 = u * u;
    return p[s] * (6.0f * u2 - 6.0f * u) + m0 * (3.0f * u2 - 4.0f * u + 1.0f) + p[e] * (6.0f * u - 6.0f * u2) + m1 * (3.0f * u2 - 2.0f * u);
}

f32 spline_length(const Vec3* p, u32 n, bool closed, const Vec3* tangents) {
    const u32 samples = spline_segments(n, closed) * kSamplesPerSegment;
    f32 total = 0.0f;
    Vec3 previous = spline_point(p, n, closed, 0.0f, tangents);
    for (u32 i = 1; i <= samples; ++i) {
        const Vec3 next = spline_point(p, n, closed, static_cast<f32>(i) / kSamplesPerSegment, tangents);
        total += length(next - previous);
        previous = next;
    }
    return total;
}

Vec3 spline_at_distance(const Vec3* p, u32 n, bool closed, f32 distance, Vec3* tangent, const Vec3* tangents) {
    const u32 samples = spline_segments(n, closed) * kSamplesPerSegment;
    f32 walked = 0.0f;
    f32 t = static_cast<f32>(spline_segments(n, closed));
    Vec3 previous = spline_point(p, n, closed, 0.0f, tangents);
    if (distance <= 0.0f) t = 0.0f;
    for (u32 i = 1; i <= samples && distance > 0.0f; ++i) {
        const Vec3 next = spline_point(p, n, closed, static_cast<f32>(i) / kSamplesPerSegment, tangents);
        const f32 step = length(next - previous);
        if (walked + step >= distance) {  // inside this sample: interpolate t linearly
            const f32 f = step > 0.0f ? (distance - walked) / step : 0.0f;
            t = (static_cast<f32>(i - 1) + f) / kSamplesPerSegment;
            break;
        }
        walked += step;
        previous = next;
    }
    if (tangent) *tangent = spline_tangent(p, n, closed, t, tangents);
    return spline_point(p, n, closed, t, tangents);
}

u32 spline_world_curve(const World& world, Entity e, Vec3 points[32], Vec3 tangents[32], bool& closed) {
    const Spline* spline = world.get<Spline>(e);
    if (!spline) return 0;
    const Mat4 m = world.world_matrix(e);
    spline_tangents(*spline, tangents);
    for (u32 i = 0; i < spline->points.count; ++i) {
        points[i] = transform_point(m, spline->points[i]);
        tangents[i] = transform_vector(m, tangents[i]);  // zero stays zero: still automatic
    }
    closed = spline->closed;
    return spline->points.count;
}

// shortcut: one hash per entity id for the process (a few bytes each); never pruned.
static std::unordered_map<u32, u64> g_spline_mesh_hashes;

void update_spline_meshes(World& world) {
    const reflect::TypeInfo& type = reflect::type_of<SplineMeshes>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const Entity e = world.owner(type, i);
        const auto* placement = static_cast<const SplineMeshes*>(world.at(type, i));
        const Spline* spline = world.get<Spline>(e);
        if (!spline || spline->points.count < 2) continue;
        auto* ism = world.get<InstancedStaticMesh>(e);
        // Skip unless the curve or the settings changed (or the copies are gone): a hash of both.
        u64 hash = 14695981039346656037ull;
        auto mix = [&hash](const void* data, usize size) {
            for (usize b = 0; b < size; ++b) hash = (hash ^ static_cast<const u8*>(data)[b]) * 1099511628211ull;
        };
        mix(spline->points.items, sizeof(Vec3) * spline->points.count);  // fields, not whole structs: padding is noise
        mix(spline->tangents.items, sizeof(Vec3) * spline->tangents.count);
        mix(&spline->closed, sizeof spline->closed);
        mix(placement->mesh, sizeof placement->mesh);
        mix(placement->material, sizeof placement->material);
        mix(&placement->color, sizeof placement->color);
        mix(&placement->spacing, sizeof placement->spacing);
        mix(&placement->follow_curve, sizeof placement->follow_curve);
        mix(&placement->scale, sizeof placement->scale);
        mix(&placement->cast_shadows, sizeof placement->cast_shadows);
        u64& seen = g_spline_mesh_hashes[e.value];
        if (ism && ism->instances.count && seen == hash) continue;
        seen = hash;
        if (!ism) ism = world.add<InstancedStaticMesh>(e);
        std::snprintf(ism->mesh, sizeof ism->mesh, "%s", placement->mesh);
        std::snprintf(ism->material, sizeof ism->material, "%s", placement->material);
        ism->color = placement->color;
        ism->cast_shadows = placement->cast_shadows;
        Vec3 tangents[32];
        spline_tangents(*spline, tangents);
        const u32 n = spline->points.count;
        const f32 total = spline_length(spline->points.items, n, spline->closed, tangents);
        const f32 spacing = placement->spacing > 0.05f ? placement->spacing : 0.05f;
        const u32 copies = static_cast<u32>(total / spacing) + (spline->closed ? 0u : 1u);
        ism->instances.count = 0;
        for (u32 k = 0; k < copies && k < ism->instances.capacity(); ++k) {
            Vec3 direction;
            MeshInstance copy;
            copy.position = spline_at_distance(spline->points.items, n, spline->closed, static_cast<f32>(k) * spacing, &direction, tangents);
            if (placement->follow_curve) {
                copy.rotation = {0.0f, -degrees(std::atan2(direction.z, std::sqrt(direction.x * direction.x + direction.y * direction.y))), degrees(std::atan2(direction.y, direction.x))};
            }
            copy.scale = placement->scale;
            ism->instances.push_back(copy);
        }
    }
}

} // namespace lsd::world