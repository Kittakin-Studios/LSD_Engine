#include "lsd/world/timeline.h"
#include "lsd/world/curve.h"

#include <algorithm>
#include <cmath>

namespace lsd::world {

f32* key_channel(TimelineKey& k, u32 c) {
    if (c < 3) return &k.position.x + c;
    if (c < 6) return &k.rotation.x + (c - 3);
    if (c == 6) return &k.value;
    return &k.color.r + (c < 10 ? c - 7 : 3);
}

bool sample_channel(const Timeline& t, u32 track, u32 channel, f32 time, f32& value) {
    // The track's keys as a curve (Auto = Smooth with automatic tangents, flat at the ends).
    // shortcut: the first 64 keys of a track, sorted every call; cache per track if timelines grow.
    CurveKeys curve;
    for (const TimelineKey& k : t.keys) {
        if (k.track != track || curve.count == curve.capacity()) continue;
        CurveKey c;
        c.time = k.time;
        c.value = key_channel(k, channel);
        c.interp = k.interp <= 2 ? k.interp : 0;
        curve.push_back(c);
    }
    if (curve.count == 0) return false;
    sort_keys(curve);
    value = evaluate(curve, time);
    return true;
}

bool sample_transform(const Timeline& t, u32 track, f32 time, Vec3& position, Vec3& rotation) {
    f32 out[6];
    for (u32 c = 0; c < 6; ++c) {
        if (!sample_channel(t, track, c, time, out[c])) return false;
    }
    position = {out[0], out[1], out[2]};
    rotation = {out[3], out[4], out[5]};  // degrees, the short way is up to the keys
    return true;
}

bool sample_value(const Timeline& t, u32 track, f32 time, f32& value) { return sample_channel(t, track, 6, time, value); }

bool sample_color(const Timeline& t, u32 track, f32 time, LinearColor& color) {
    for (u32 c = 0; c < 4; ++c) {
        if (!sample_channel(t, track, 7 + c, time, (&color.r)[c])) return false;
    }
    return true;
}

namespace {

// The last key at or before `time` on tracks of `type` (with a camera, when `camera`).
const TimelineKey* last_key(const Timeline& t, TrackType type, f32 time, bool camera) {
    const TimelineKey* best = nullptr;
    for (const TimelineKey& k : t.keys) {
        if (k.track >= t.tracks.count || t.tracks[k.track].type != static_cast<u32>(type) || k.time > time + 1e-5f) continue;
        if (camera && !k.text[0]) continue;
        if (!best || k.time >= best->time) best = &k;
    }
    return best;
}

} // namespace

const TimelineKey* shot_at(const Timeline& t, f32 time) { return last_key(t, TrackType::Shot, time, false); }

const char* camera_at(const Timeline& t, f32 time) {
    const TimelineKey* cut = last_key(t, TrackType::CameraCut, time, true);
    const TimelineKey* shot = last_key(t, TrackType::Shot, time, true);
    const TimelineKey* k = !cut ? shot : !shot ? cut : (shot->time > cut->time ? shot : cut);  // the later one wins
    return k ? k->text : nullptr;
}

f32 cine_fov_degrees(f32 sensor_height_mm, f32 focal_length_mm) {
    if (focal_length_mm <= 0.0f) return 60.0f;
    return 2.0f * std::atan(sensor_height_mm * 0.5f / focal_length_mm) * 57.29578f;
}

LensBlur cine_depth_of_field(f32 focal_length_mm, f32 sensor_width_mm, f32 sensor_height_mm, f32 aperture, f32 focus_m, f32 image_height) {
    LensBlur out;
    if (focal_length_mm <= 0.0f || aperture <= 0.0f || focus_m <= 0.0f || sensor_height_mm <= 0.0f) return out;
    const f32 f = focal_length_mm * 0.001f;
    const f32 coc = sensor_width_mm * 0.001f / 1440.0f;  // acceptable blur circle: 0.025 mm on a 36 mm film back
    const f32 hyperfocal = f * f / (aperture * coc) + f;
    const f32 s = std::max(focus_m, f * 1.01f);
    const f32 near_edge = s * (hyperfocal - f) / (hyperfocal + s - 2.0f * f);
    // shortcut: one symmetric sharp region (the near side, the narrower one) because the post pass has one; give it near / far when it does.
    out.focal_region = std::clamp(s - near_edge, 0.0f, 1000.0f);
    out.transition = std::clamp(s * 0.5f, 0.05f, 1000.0f);  // blur grows over about half the focus distance
    // Blur circle of something far away, on the sensor -> radius in pixels of the image.
    const f32 coc_far = f * f / (aperture * (s - f));
    out.max_blur = std::clamp(coc_far / (sensor_height_mm * 0.001f) * image_height * 0.5f, 0.0f, 32.0f);
    return out;
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
