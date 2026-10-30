#include "lsd/world/curve.h"

namespace lsd::world {

namespace {

// Automatic tangent (value per second) at key i: the slope across its neighbours, flat at the ends.
f32 tangent(const Curve& c, u32 i) {
    if (i == 0 || i + 1 >= c.keys.count) return 0.0f;
    const CurveKey& a = c.keys[i - 1];
    const CurveKey& b = c.keys[i + 1];
    return b.time > a.time ? (b.value - a.value) / (b.time - a.time) : 0.0f;
}

} // namespace

f32 evaluate(const Curve& c, f32 time) {
    const u32 n = c.keys.count;
    if (n == 0) return 0.0f;
    if (time <= c.keys[0].time) return c.keys[0].value;
    if (time >= c.keys[n - 1].time) return c.keys[n - 1].value;
    u32 i = 0;
    while (i + 1 < n && c.keys[i + 1].time < time) ++i;
    const CurveKey& a = c.keys[i];
    const CurveKey& b = c.keys[i + 1];
    const f32 span = b.time - a.time;
    if (span <= 0.0f || a.interp == 2) return a.value;  // Constant (or two keys at one time)
    const f32 u = (time - a.time) / span;
    if (a.interp == 1) return a.value + (b.value - a.value) * u;  // Linear
    // Cubic Hermite with automatic tangents.
    const f32 u2 = u * u, u3 = u2 * u;
    const f32 h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    return h00 * a.value + h10 * span * tangent(c, i) + h01 * b.value + h11 * span * tangent(c, i + 1);
}

void sort_keys(Curve& c) {
    for (u32 i = 1; i < c.keys.count; ++i) {  // insertion sort: a handful of keys, usually nearly sorted
        const CurveKey key = c.keys[i];
        u32 j = i;
        while (j > 0 && c.keys[j - 1].time > key.time) {
            c.keys[j] = c.keys[j - 1];
            --j;
        }
        c.keys[j] = key;
    }
}

} // namespace lsd::world
