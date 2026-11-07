#include "lsd/animation/motion_matching.h"

#include "lsd/foundation/log.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lsd::anim {

namespace {

f32 wrap_angle(f32 a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

void lower(const char* in, char* out, usize capacity) {
    usize i = 0;
    for (; in[i] && i + 1 < capacity; ++i) out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(in[i])));
    out[i] = '\0';
}

// A joint by its name, or else the first whose name looks like one (foot_l, LeftFoot, mixamorig:LeftFoot, CC_Base_L_Foot...).
i32 find_bone(const SkinnedModel& model, const char* given, bool (*looks)(const char* lowered)) {
    if (given && given[0]) return model.find_joint(given);
    for (u32 j = 0; j < model.joint_count(); ++j) {
        char name[96];
        lower(model.joint_name(j), name, sizeof name);
        if (looks(name)) return static_cast<i32>(j);
    }
    return -1;
}
bool not_helper(const char* n) { return !std::strstr(n, "toe") && !std::strstr(n, "ball") && !std::strstr(n, "ik") && !std::strstr(n, "twist"); }
bool ends(const char* n, const char* suffix) {
    const usize a = std::strlen(n), b = std::strlen(suffix);
    return a >= b && std::strcmp(n + a - b, suffix) == 0;
}
bool left_foot(const char* n) {
    return std::strstr(n, "foot") && not_helper(n) && (std::strstr(n, "left") || ends(n, "_l") || ends(n, ".l") || std::strstr(n, "_l_") || std::strncmp(n, "l_", 2) == 0);
}
bool right_foot(const char* n) {
    return std::strstr(n, "foot") && not_helper(n) && (std::strstr(n, "right") || ends(n, "_r") || ends(n, ".r") || std::strstr(n, "_r_") || std::strncmp(n, "r_", 2) == 0);
}
bool hips(const char* n) { return (std::strstr(n, "pelvis") || std::strstr(n, "hips")) && !std::strstr(n, "ik"); }
bool root_bone(const char* n) { return std::strcmp(n, "root") == 0; }  // Unreal rigs' root; our own scene root is "Root" (joint_of), never animated

Vec3 position_of(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }
f32 yaw_of(const Mat4& m) { return std::atan2(m.at(1, 0), m.at(0, 0)); }
Vec3 rotate_z(Vec3 v, f32 angle) {
    const f32 c = std::cos(angle), s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

} // namespace

bool MotionData::build(const SkinnedModel& model, const Clip* const* source, const bool* loops, u32 count, const MotionDatabase& settings, f32 model_forward_yaw) {
    features.clear();
    frames.clear();
    clip_count = 0;
    forward_yaw = model_forward_yaw;
    for (bool& m : mirrored) m = false;
    const i32 foot_l = find_bone(model, settings.left_foot, left_foot);
    const i32 foot_r = find_bone(model, settings.right_foot, right_foot);
    const i32 hip = find_bone(model, settings.hips, hips);
    const i32 root = find_bone(model, "", root_bone);
    if (foot_l < 0 || foot_r < 0 || hip < 0) {
        LSD_LOG_WARN("anim", "Motion matching: no %s bone found (set it in the Motion Database)", foot_l < 0 ? "left foot" : foot_r < 0 ? "right foot" : "hips");
        return false;
    }
    std::vector<f32> raw;  // un-normalised features, frame after frame
    Pose pose;
    const f32 step = 1.0f / kMotionFrameRate;
    for (u32 c = 0; c < count && clip_count < 64; ++c) {
        const Clip* clip = source[c];
        if (!clip || clip->duration <= 0.0f) continue;
        const u32 n = static_cast<u32>(clip->duration * kMotionFrameRate) + 1;
        std::vector<Vec3> fl(n), fr(n), hp(n), rt(n);
        std::vector<f32> ryaw(n);
        for (u32 k = 0; k < n; ++k) {
            pose.sample(model, clip, std::fmin(static_cast<f32>(k) * step, clip->duration), false);
            fl[k] = position_of(pose.joint_model(static_cast<u32>(foot_l)));
            fr[k] = position_of(pose.joint_model(static_cast<u32>(foot_r)));
            hp[k] = position_of(pose.joint_model(static_cast<u32>(hip)));
            const Mat4 r = root >= 0 ? pose.joint_model(static_cast<u32>(root)) : Mat4{};
            rt[k] = position_of(r);
            ryaw[k] = yaw_of(r);
        }
        // The character's root over the clip: the root bone when it carries the motion, else the
        // hips when they travel (root motion baked in them), else made from the planted foot (in place).
        auto travel = [&](const std::vector<Vec3>& p) { return std::sqrt((p[n - 1].x - p[0].x) * (p[n - 1].x - p[0].x) + (p[n - 1].y - p[0].y) * (p[n - 1].y - p[0].y)); };
        const bool root_motion = root >= 0 && (travel(rt) > 0.05f || std::fabs(wrap_angle(ryaw[n - 1] - ryaw[0])) > 0.1f);
        const bool hips_motion = !root_motion && travel(hp) > 0.3f;
        const bool in_place = !root_motion && !hips_motion;
        auto deriv = [&](const std::vector<Vec3>& p, u32 k) {
            const u32 a = k > 0 ? k - 1 : k, b = k + 1 < n ? k + 1 : k;
            return b > a ? (p[b] - p[a]) * (kMotionFrameRate / static_cast<f32>(b - a)) : Vec3{};
        };
        std::vector<Vec3> root_pos(n), root_vel(n);
        std::vector<f32> facing(n);
        for (u32 k = 0; k < n; ++k) {
            facing[k] = model_forward_yaw + (root_motion ? wrap_angle(ryaw[k] - ryaw[0]) : 0.0f);
            if (in_place) {  // the planted (lower) foot stands still in the world: the body moves the other way
                const Vec3 v = fl[k].z < fr[k].z ? deriv(fl, k) : deriv(fr, k);
                root_vel[k] = {-v.x, -v.y, 0.0f};
            }
        }
        if (in_place) {
            for (u32 k = 0; k < n; ++k) {  // smoothed over 5 frames (foot swaps flicker)
                Vec3 sum{};
                u32 used = 0;
                for (i32 d = -2; d <= 2; ++d) {
                    const i32 j = static_cast<i32>(k) + d;
                    if (j < 0 || j >= static_cast<i32>(n)) continue;
                    sum = sum + root_vel[static_cast<u32>(j)];
                    ++used;
                }
                root_pos[k] = sum * (1.0f / static_cast<f32>(used));  // (reused as the smoothed velocity first)
            }
            root_vel.swap(root_pos);
            root_pos[0] = {};
            for (u32 k = 1; k < n; ++k) root_pos[k] = root_pos[k - 1] + root_vel[k] * step;
        } else {
            const std::vector<Vec3>& src = root_motion ? rt : hp;
            for (u32 k = 0; k < n; ++k) root_pos[k] = {src[k].x, src[k].y, 0.0f};
            for (u32 k = 0; k < n; ++k) root_vel[k] = deriv(root_pos, k);
        }
        // The root where `j` frames are ahead of the clip's start (past its end: wrapped for loops, carried on otherwise).
        const Vec3 cycle = root_pos[n - 1] - root_pos[0];
        const f32 cycle_turn = wrap_angle(facing[n - 1] - facing[0]);
        auto ahead = [&](i64 j, Vec3& at, f32& face) {
            if (j < static_cast<i64>(n)) {
                at = root_pos[static_cast<usize>(j)];
                face = facing[static_cast<usize>(j)];
            } else if (loops[c] && n > 1) {
                const i64 laps = j / static_cast<i64>(n - 1), within = j % static_cast<i64>(n - 1);
                at = root_pos[static_cast<usize>(within)] + cycle * static_cast<f32>(laps);
                face = facing[static_cast<usize>(within)] + cycle_turn * static_cast<f32>(laps);
            } else {
                at = root_pos[n - 1] + root_vel[n - 1] * (static_cast<f32>(j - static_cast<i64>(n - 1)) * step);
                face = facing[n - 1];
            }
        };
        clip_first[clip_count] = static_cast<u32>(frames.size());
        for (u32 k = 0; k < n; ++k) {
            const f32 turn = -facing[k];  // world-ish (model) space into the character's own
            const Vec3 base = root_pos[k];
            const Vec3 extra = in_place ? root_vel[k] : Vec3{};  // in-place clips: the body's motion added to the parts'
            f32 f[kMotionFeatures];
            auto put = [&](u32 at, Vec3 v) {
                f[at] = v.x;
                f[at + 1] = v.y;
                f[at + 2] = v.z;
            };
            put(0, rotate_z(fl[k] - Vec3{base.x, base.y, 0.0f}, turn));
            put(3, rotate_z(fr[k] - Vec3{base.x, base.y, 0.0f}, turn));
            put(6, rotate_z(deriv(fl, k) + extra, turn));
            put(9, rotate_z(deriv(fr, k) + extra, turn));
            put(12, rotate_z(deriv(hp, k) + extra, turn));
            for (u32 t = 0; t < 3; ++t) {
                Vec3 at;
                f32 face;
                ahead(static_cast<i64>(k) + static_cast<i64>(std::lround(kMotionTrajectoryTimes[t] * kMotionFrameRate)), at, face);
                const Vec3 rel = rotate_z(at - base, turn);
                f[15 + t * 2] = rel.x;
                f[16 + t * 2] = rel.y;
                f[21 + t * 2] = std::cos(face - facing[k]);
                f[22 + t * 2] = std::sin(face - facing[k]);
            }
            raw.insert(raw.end(), f, f + kMotionFeatures);
            frames.push_back({clip_count, std::fmin(static_cast<f32>(k) * step, clip->duration)});
        }
        clip_frames[clip_count] = n;
        clips[clip_count] = clip;
        looping[clip_count] = loops[c];
        std::snprintf(names[clip_count], sizeof names[0], "%s", clip->name);
        ++clip_count;
        if (settings.mirror && settings.blend_mode == 1 && clip_count < 64) {
            // Its mirror, straight from the features (the character's own space: x ahead, y to the left):
            // sideways values change sign, the feet swap, turns go the other way.
            const usize first = raw.size() - static_cast<usize>(n) * kMotionFeatures;
            clip_first[clip_count] = static_cast<u32>(frames.size());
            for (u32 k = 0; k < n; ++k) {
                f32 f[kMotionFeatures];
                std::memcpy(f, &raw[first + static_cast<usize>(k) * kMotionFeatures], sizeof f);
                for (u32 a = 0; a < 3; ++a) {
                    std::swap(f[a], f[3 + a]);
                    std::swap(f[6 + a], f[9 + a]);
                }
                for (const u32 y : {1u, 4u, 7u, 10u, 13u, 16u, 18u, 20u, 22u, 24u, 26u}) f[y] = -f[y];
                raw.insert(raw.end(), f, f + kMotionFeatures);
                frames.push_back({clip_count, frames[clip_first[clip_count - 1] + k].time});
            }
            clip_frames[clip_count] = n;
            clips[clip_count] = clip;
            looping[clip_count] = loops[c];
            mirrored[clip_count] = true;
            std::snprintf(names[clip_count], sizeof names[0], "%.52s (mirrored)", clip->name);
            ++clip_count;
        }
    }
    const usize count_frames = frames.size();
    if (!count_frames) return false;
    // Normalise each group (feet positions, feet velocities, hips velocity, path, facing) by its own
    // spread, so no group wins by its units, then weigh the groups.
    const u32 groups[6] = {0, 6, 12, 15, 21, 27};
    const f32 weights[5] = {settings.pose_weight, settings.pose_weight, settings.pose_weight, settings.trajectory_weight, settings.facing_weight};
    for (u32 d = 0; d < kMotionFeatures; ++d) {
        f64 sum = 0;
        for (usize i = 0; i < count_frames; ++i) sum += raw[i * kMotionFeatures + d];
        mean[d] = static_cast<f32>(sum / static_cast<f64>(count_frames));
    }
    for (u32 g = 0; g < 5; ++g) {
        f64 variance = 0;
        for (u32 d = groups[g]; d < groups[g + 1]; ++d) {
            for (usize i = 0; i < count_frames; ++i) {
                const f64 x = raw[i * kMotionFeatures + d] - mean[d];
                variance += x * x;
            }
        }
        const f32 spread = static_cast<f32>(std::sqrt(variance / static_cast<f64>(count_frames * (groups[g + 1] - groups[g]))));
        for (u32 d = groups[g]; d < groups[g + 1]; ++d) scale[d] = weights[g] / (spread > 1e-3f ? spread : 1e-3f);
    }
    features.resize(raw.size());
    for (usize i = 0; i < count_frames; ++i) {
        for (u32 d = 0; d < kMotionFeatures; ++d) features[i * kMotionFeatures + d] = (raw[i * kMotionFeatures + d] - mean[d]) * scale[d];
    }
    return true;
}

void predict_trajectory(const MotionInput& input, f32 positions[6], f32 facings[6]) {
    // Velocity and facing close in on what is asked like a critically damped spring (about 0.2 s to half way).
    constexpr f32 kVelocityRate = 4.0f, kFacingRate = 6.0f;
    const Vec3 v0 = rotate_z(input.velocity, -input.facing);
    const Vec3 vd = rotate_z(input.desired_velocity, -input.facing);
    const f32 turn = wrap_angle(input.desired_facing - input.facing);
    for (u32 t = 0; t < 3; ++t) {
        const f32 T = kMotionTrajectoryTimes[t];
        const f32 decay = (1.0f - std::exp(-kVelocityRate * T)) / kVelocityRate;
        positions[t * 2] = vd.x * T + (v0.x - vd.x) * decay;
        positions[t * 2 + 1] = vd.y * T + (v0.y - vd.y) * decay;
        const f32 face = turn * (1.0f - std::exp(-kFacingRate * T));
        facings[t * 2] = std::cos(face);
        facings[t * 2 + 1] = std::sin(face);
    }
}

u32 search_motion(const MotionData& data, const f32* query, f32* cost) {
    // shortcut: brute force (fine for a few thousand frames); a k-d tree or clustering when databases grow to minutes of motion
    u32 best = 0;
    f32 best_cost = 1e30f;
    const usize n = data.frames.size();
    for (usize i = 0; i < n; ++i) {
        const f32* f = &data.features[i * kMotionFeatures];
        f32 d = 0.0f;
        for (u32 k = 0; k < kMotionFeatures && d < best_cost; ++k) d += (f[k] - query[k]) * (f[k] - query[k]);
        if (d < best_cost) {
            best_cost = d;
            best = static_cast<u32>(i);
        }
    }
    if (cost) *cost = best_cost;
    return best;
}

u32 step_motion(const MotionData& data, const MotionDatabase& settings, const MotionInput& input, MotionState& s, f32 dt, BlendInput* out) {
    if (data.frames.empty()) return 0;
    auto duration = [&](i32 clip) { return data.clips[clip]->duration; };
    auto advance = [&](i32 clip, f32& time) {  // true when a one-shot clip ran out
        time += dt;
        if (time <= duration(clip)) return false;
        if (data.looping[clip]) {
            time = std::fmod(time, duration(clip));
            return false;
        }
        time = duration(clip);
        return true;
    };
    bool ran_out = false;
    s.switched = false;
    const bool inertial = settings.blend_mode == 1;
    if (s.clip >= 0) {
        ran_out = advance(s.clip, s.time);
        if (s.previous_clip >= 0) advance(s.previous_clip, s.previous_time);
        s.fade = settings.blend_time > 0.0f ? std::fmin(s.fade + dt / settings.blend_time, 1.0f) : 1.0f;
    }
    s.since_search += dt;
    if (s.clip < 0 || ran_out || s.since_search >= settings.search_interval) {
        s.since_search = 0.0f;
        f32 query[kMotionFeatures] = {};  // the pose part: the frame playing now (none yet: the average pose)
        if (s.clip >= 0) {
            const u32 at = data.clip_first[s.clip] + std::min(static_cast<u32>(s.time * kMotionFrameRate + 0.5f), data.clip_frames[s.clip] - 1);
            std::memcpy(query, &data.features[at * kMotionFeatures], kMotionPoseFeatures * sizeof(f32));
        }
        f32 positions[6], facings[6];
        predict_trajectory(input, positions, facings);
        for (u32 k = 0; k < 6; ++k) {
            query[15 + k] = (positions[k] - data.mean[15 + k]) * data.scale[15 + k];
            query[21 + k] = (facings[k] - data.mean[21 + k]) * data.scale[21 + k];
        }
        const u32 best = search_motion(data, query, &s.cost);
        const MotionFrame& f = data.frames[best];
        f32 gap = std::fabs(f.time - s.time);
        if (s.clip >= 0 && data.looping[s.clip]) gap = std::fmin(gap, duration(s.clip) - gap);
        if (s.clip < 0) {
            s.clip = static_cast<i32>(f.clip);
            s.time = f.time;
            s.fade = 1.0f;
        } else if (!(static_cast<i32>(f.clip) == s.clip && gap < settings.same_clip_window)) {
            s.previous_clip = s.clip;
            s.previous_time = s.time;
            s.clip = static_cast<i32>(f.clip);
            s.time = f.time;
            s.fade = settings.blend_time > 0.0f && !inertial ? 0.0f : 1.0f;
            s.switched = inertial;
        }
    }
    u32 n = 0;
    if (s.fade < 1.0f && s.previous_clip >= 0) out[n++] = {data.clips[s.previous_clip], s.previous_time / duration(s.previous_clip), 1.0f - s.fade, data.mirrored[s.previous_clip]};
    out[n++] = {data.clips[s.clip], s.time / duration(s.clip), s.fade < 1.0f && s.previous_clip >= 0 ? s.fade : 1.0f, data.mirrored[s.clip]};
    return n;
}

} // namespace lsd::anim
