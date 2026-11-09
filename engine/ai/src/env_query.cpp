#include "lsd/ai/env_query.h"

#include <cmath>

namespace lsd::ai {

namespace {

u32 generate(const EnvQuery& q, Vec3 centre, QueryPoint* out, u32 capacity) {
    u32 n = 0;
    if (q.generator == QueryGenerator::Rings) {
        const u32 rings = q.rings ? q.rings : 1;
        for (u32 r = 1; r <= rings; ++r) {
            const f32 radius = q.radius * static_cast<f32>(r) / static_cast<f32>(rings);
            for (u32 i = 0; i < q.points_per_ring && n < capacity; ++i) {
                const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(q.points_per_ring);
                out[n++] = {centre + Vec3{std::cos(a) * radius, std::sin(a) * radius, 0.0f}, 0.0f, true};
            }
        }
    } else {
        const f32 step = q.spacing > 0.1f ? q.spacing : 0.1f;
        const i32 half = static_cast<i32>(q.radius / step);
        for (i32 y = -half; y <= half; ++y) {
            for (i32 x = -half; x <= half && n < capacity; ++x) {
                out[n++] = {centre + Vec3{static_cast<f32>(x) * step, static_cast<f32>(y) * step, 0.0f}, 0.0f, true};
            }
        }
    }
    return n;
}

f32 measure(const QueryTest& t, const QueryHost& host, Vec3 point, Vec3 querier, Vec3 forward, Vec3 target) {
    switch (t.type) {
        case QueryTestType::Distance: return length(point - (t.to_target ? target : querier));
        case QueryTestType::Visible: return host.visible && host.visible(host.user, point, target) ? 1.0f : 0.0f;
        case QueryTestType::Reachable: return !host.reachable || host.reachable(host.user, querier, point) ? 1.0f : 0.0f;
        case QueryTestType::Facing: {
            Vec3 d = point - querier;
            d.z = 0.0f;
            const f32 len = length(d);
            return len > 1e-4f ? dot(d * (1.0f / len), forward) : 0.0f;  // its own spot: neither ahead nor behind
        }
    }
    return 0.0f;
}

} // namespace

i32 run_query(const EnvQuery& q, const QueryHost& host, Vec3 querier, Vec3 forward, Vec3 target, QueryPoint* out, u32 capacity, u32& count) {
    capacity = capacity < kMaxQueryPoints ? capacity : kMaxQueryPoints;
    count = generate(q, q.around_target ? target : querier, out, capacity);
    if (q.on_navmesh && host.project) {
        for (u32 i = 0; i < count; ++i) {
            Vec3 snapped;
            if (host.project(host.user, out[i].at, snapped)) out[i].at = snapped;
            else out[i].kept = false;
        }
    }
    f32 values[kMaxQueryPoints];
    for (u32 t = 0; t < q.test_count && t < kMaxQueryTests; ++t) {
        const QueryTest& test = q.tests[t];
        const bool binary = test.type == QueryTestType::Visible || test.type == QueryTestType::Reachable;
        f32 lo = 1e30f, hi = -1e30f;
        for (u32 i = 0; i < count; ++i) {
            if (!out[i].kept) continue;
            values[i] = measure(test, host, out[i].at, querier, forward, target);
            if (test.filter && (binary ? values[i] < 0.5f : (values[i] < test.min || values[i] > test.max))) {
                out[i].kept = false;
                continue;
            }
            lo = values[i] < lo ? values[i] : lo;
            hi = values[i] > hi ? values[i] : hi;
        }
        if (!test.score) continue;
        // Normalised over the points still in (0 worst .. 1 best for a positive weight), so tests of different units mix.
        const f32 range = binary ? 1.0f : hi - lo;
        for (u32 i = 0; i < count; ++i) {
            if (!out[i].kept) continue;
            const f32 norm = binary ? values[i] : range > 1e-6f ? (values[i] - lo) / range : 1.0f;
            out[i].score += test.weight >= 0.0f ? norm * test.weight : (1.0f - norm) * -test.weight;
        }
    }
    i32 best = -1;
    for (u32 i = 0; i < count; ++i) {
        if (out[i].kept && (best < 0 || out[i].score > out[best].score)) best = static_cast<i32>(i);
    }
    return best;
}

} // namespace lsd::ai
