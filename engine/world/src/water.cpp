#include "lsd/world/water.h"

#include "lsd/world/spline.h"

#include <cmath>

namespace lsd::world {

namespace {

// The smaller waves: turned from the main direction, shorter (amplitude in step, so all are as steep)
// and slower (deep water: speed grows with the square root of the wavelength).
constexpr f32 kTurn[kWaterWaves] = {0.0f, 31.0f, -43.0f, 77.0f};
constexpr f32 kScale[kWaterWaves] = {1.0f, 0.61f, 0.37f, 0.23f};

} // namespace

void water_waves(const WaterBody& w, f32 out[kWaterWaves][4]) {
    for (u32 k = 0; k < kWaterWaves; ++k) {
        const f32 angle = radians(w.wave_direction + kTurn[k]);
        const f32 length = std::fmax(w.wavelength, 0.5f) * kScale[k];
        const f32 number = 6.2831853f / length;
        out[k][0] = std::cos(angle) * number;
        out[k][1] = std::sin(angle) * number;
        out[k][2] = std::fmax(w.wave_height, 0.0f) * 0.5f * kScale[k];
        out[k][3] = number * std::fmax(w.wave_speed, 0.0f) * std::sqrt(kScale[k]);
    }
}

f32 water_wave_height(const WaterBody& w, f32 x, f32 y, f32 time, f32* slope_x, f32* slope_y) {
    f32 waves[kWaterWaves][4];
    water_waves(w, waves);
    f32 h = 0.0f, sx = 0.0f, sy = 0.0f;
    for (const auto& wave : waves) {
        const f32 phase = wave[0] * x + wave[1] * y - wave[3] * time;
        h += wave[2] * std::sin(phase);
        const f32 c = wave[2] * std::cos(phase);
        sx += c * wave[0];
        sy += c * wave[1];
    }
    if (slope_x) *slope_x = sx;
    if (slope_y) *slope_y = sy;
    return h;
}

u32 river_samples(const World& world, Entity e, Vec3 points[kRiverSamples], Vec3 left[kRiverSamples]) {
    Vec3 curve[32], tangents[32];
    bool closed = false;
    const u32 n = spline_world_curve(world, e, curve, tangents, closed);
    if (n < 2) return 0;
    const f32 length = spline_length(curve, n, closed, tangents);
    u32 count = static_cast<u32>(length) + 2;  // about a metre apart
    if (count > kRiverSamples) count = kRiverSamples;
    for (u32 i = 0; i < count; ++i) {
        Vec3 along{1, 0, 0};
        points[i] = spline_at_distance(curve, n, closed, length * static_cast<f32>(i) / static_cast<f32>(count - 1), &along, tangents);
        const f32 flat = std::sqrt(along.x * along.x + along.y * along.y);
        left[i] = flat > 1e-6f ? Vec3{-along.y / flat, along.x / flat, 0.0f} : Vec3{0, 1, 0};
    }
    return count;
}

bool water_surface(const World& world, Entity e, Vec3 p, f32 time, f32& height) {
    const WaterBody* w = world.get<WaterBody>(e);
    if (!w) return false;
    const Mat4 m = world.world_matrix(e);
    f32 rest = 0.0f;
    if (w->kind == 1) {  // ocean: everywhere, at the object's height
        rest = m.at(2, 3);
    } else if (w->kind == 2) {  // river: the nearest stretch of the curve, within half the width
        Vec3 points[kRiverSamples], left[kRiverSamples];
        const u32 n = river_samples(world, e, points, left);
        f32 best = 1e30f;
        for (u32 i = 0; i + 1 < n; ++i) {
            const f32 dx = points[i + 1].x - points[i].x, dy = points[i + 1].y - points[i].y;
            const f32 len2 = dx * dx + dy * dy;
            f32 t = len2 > 1e-8f ? ((p.x - points[i].x) * dx + (p.y - points[i].y) * dy) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            const f32 ox = p.x - (points[i].x + dx * t), oy = p.y - (points[i].y + dy * t);
            const f32 d2 = ox * ox + oy * oy;
            if (d2 < best) {
                best = d2;
                rest = points[i].z + (points[i + 1].z - points[i].z) * t;
            }
        }
        const f32 half = w->river_width * 0.5f;
        if (n < 2 || best > half * half) return false;
    } else {  // lake: the rectangle in the object's own space
        const Vec3 local = transform_point(affine_inverse(m), p);
        if (std::fabs(local.x) > w->size_x * 0.5f || std::fabs(local.y) > w->size_y * 0.5f) return false;
        rest = transform_point(m, Vec3{local.x, local.y, 0.0f}).z;
    }
    height = rest + water_wave_height(*w, p.x, p.y, time);
    return true;
}

bool in_water(const World& world, Vec3 p, f32 time, f32* surface, Entity* body) {
    const reflect::TypeInfo& type = reflect::type_of<WaterBody>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const Entity e = world.owner(type, i);
        f32 h = 0.0f;
        if (!water_surface(world, e, p, time, h) || p.z > h || p.z < h - static_cast<const WaterBody*>(world.at(type, i))->depth) continue;
        if (surface) *surface = h;
        if (body) *body = e;
        return true;
    }
    return false;
}

} // namespace lsd::world
