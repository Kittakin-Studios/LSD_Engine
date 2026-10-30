#include "lsd/world/spline.h"

namespace lsd::world {

namespace {

constexpr u32 kSamplesPerSegment = 16;

// The four points around segment `s`; ends repeat the first / last point when open.
void segment_points(const Vec3* p, u32 n, bool closed, u32 s, Vec3 out[4]) {
    for (int k = 0; k < 4; ++k) {
        i64 i = static_cast<i64>(s) + k - 1;
        if (closed) i = ((i % static_cast<i64>(n)) + static_cast<i64>(n)) % static_cast<i64>(n);
        else i = i < 0 ? 0 : i >= static_cast<i64>(n) ? static_cast<i64>(n) - 1 : i;
        out[k] = p[i];
    }
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

Vec3 spline_point(const Vec3* p, u32 n, bool closed, f32 t) {
    if (n == 0) return {};
    if (n == 1) return p[0];
    u32 s;
    f32 u;
    locate(n, closed, t, s, u);
    Vec3 c[4];
    segment_points(p, n, closed, s, c);
    const f32 u2 = u * u, u3 = u2 * u;
    // Uniform Catmull-Rom.
    return (c[1] * 2.0f + (c[2] - c[0]) * u + (c[0] * 2.0f - c[1] * 5.0f + c[2] * 4.0f - c[3]) * u2 + (c[1] * 3.0f - c[0] - c[2] * 3.0f + c[3]) * u3) * 0.5f;
}

Vec3 spline_tangent(const Vec3* p, u32 n, bool closed, f32 t) {
    if (n < 2) return {1, 0, 0};
    u32 s;
    f32 u;
    locate(n, closed, t, s, u);
    Vec3 c[4];
    segment_points(p, n, closed, s, c);
    return ((c[2] - c[0]) + (c[0] * 2.0f - c[1] * 5.0f + c[2] * 4.0f - c[3]) * (2.0f * u) + (c[1] * 3.0f - c[0] - c[2] * 3.0f + c[3]) * (3.0f * u * u)) * 0.5f;
}

f32 spline_length(const Vec3* p, u32 n, bool closed) {
    const u32 samples = spline_segments(n, closed) * kSamplesPerSegment;
    f32 total = 0.0f;
    Vec3 previous = spline_point(p, n, closed, 0.0f);
    for (u32 i = 1; i <= samples; ++i) {
        const Vec3 next = spline_point(p, n, closed, static_cast<f32>(i) / kSamplesPerSegment);
        total += length(next - previous);
        previous = next;
    }
    return total;
}

Vec3 spline_at_distance(const Vec3* p, u32 n, bool closed, f32 distance, Vec3* tangent) {
    const u32 samples = spline_segments(n, closed) * kSamplesPerSegment;
    f32 walked = 0.0f;
    f32 t = static_cast<f32>(spline_segments(n, closed));
    Vec3 previous = spline_point(p, n, closed, 0.0f);
    if (distance <= 0.0f) t = 0.0f;
    for (u32 i = 1; i <= samples && distance > 0.0f; ++i) {
        const Vec3 next = spline_point(p, n, closed, static_cast<f32>(i) / kSamplesPerSegment);
        const f32 step = length(next - previous);
        if (walked + step >= distance) {  // inside this sample: interpolate t linearly
            const f32 f = step > 0.0f ? (distance - walked) / step : 0.0f;
            t = (static_cast<f32>(i - 1) + f) / kSamplesPerSegment;
            break;
        }
        walked += step;
        previous = next;
    }
    if (tangent) *tangent = spline_tangent(p, n, closed, t);
    return spline_point(p, n, closed, t);
}

} // namespace lsd::world
