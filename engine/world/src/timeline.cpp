#include "lsd/world/timeline.h"

#include <algorithm>

namespace lsd::world {

namespace {

// The keys on either side of `time` and how far between them (eased); false when there are none.
bool around(const Timeline& t, u32 track, f32 time, const TimelineKey*& a, const TimelineKey*& b, f32& f) {
    a = b = nullptr;
    for (const TimelineKey& k : t.keys) {
        if (k.track != track) continue;
        if (k.time <= time && (!a || k.time >= a->time)) a = &k;
        if (k.time > time && (!b || k.time < b->time)) b = &k;
    }
    if (!a && !b) return false;
    if (!a) a = b;
    if (!b) b = a;
    const f32 span = b->time - a->time;
    f = span > 1e-6f ? std::clamp((time - a->time) / span, 0.0f, 1.0f) : 0.0f;
    f = f * f * (3.0f - 2.0f * f);  // ease in and out, like Sequencer's default auto tangents at rest
    return true;
}

} // namespace

bool sample_transform(const Timeline& t, u32 track, f32 time, Vec3& position, Vec3& rotation) {
    const TimelineKey *a, *b;
    f32 f;
    if (!around(t, track, time, a, b, f)) return false;
    position = a->position + (b->position - a->position) * f;
    rotation = a->rotation + (b->rotation - a->rotation) * f;  // degrees, the short way is up to the keys
    return true;
}

bool sample_value(const Timeline& t, u32 track, f32 time, f32& value) {
    const TimelineKey *a, *b;
    f32 f;
    if (!around(t, track, time, a, b, f)) return false;
    value = a->value + (b->value - a->value) * f;
    return true;
}

u32 keys_crossed(const Timeline& t, u32 track, f32 from, f32 to, const TimelineKey** out, u32 capacity) {
    u32 n = 0;
    for (const TimelineKey& k : t.keys) {
        if (k.track == track && k.time > from && k.time <= to && n < capacity) out[n++] = &k;
    }
    std::sort(out, out + n, [](const TimelineKey* x, const TimelineKey* y) { return x->time < y->time; });
    return n;
}

} // namespace lsd::world
