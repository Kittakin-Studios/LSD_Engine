#pragma once
// Motion matching (the technique Unreal's Motion Matching and its Game Animation Sample use): instead
// of a hand-made state machine, every frame of a set of locomotion clips is described by a few numbers
// (where the feet are and how they move, how the hips move, where the character goes over the next
// second and which way it faces), and a few times a second the frame that best matches the current
// pose plus the path the player's input asks for is found and blended to. Movement still comes from
// the character's capsule; the animation follows it.
#include "lsd/animation/animation.h"
#include "lsd/reflect/reflect.h"

namespace lsd::anim {

// One clip of a Motion Database (Unreal's Pose Search Database entry).
LSD_STRUCT()
struct MotionClip {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The FBX holding the animation (matched to the character's skeleton by bone names)") char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation (take) in that file, from 0") u32 clip = 0;
    LSD_PROPERTY(Edit, Tooltip = "It repeats (walk and run cycles, idles); off: starts, stops, turns") bool looping = true;
};

// The clips motion matching picks from, and how it weighs a match (Unreal's Pose Search Database and Schema).
LSD_STRUCT(Category = "Animation")
struct MotionDatabase {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Locomotion clips: idles, walk / jog / run cycles, starts, stops, turns, strafes. Empty: those next to the character's mesh, found by name")
    List<MotionClip, 64> clips;
    LSD_PROPERTY(Edit, Units = "s", Min = 0.02, Max = 1, Tooltip = "How often it looks for a better frame") f32 search_interval = 0.1f;
    LSD_PROPERTY(Edit, Widget = "Choice:Crossfade,Inertial", Tooltip = "Inertial (Unreal's Inertialization): jumps to the new frame at once and eases out the difference, so it reacts sooner and never mixes two clips. Crossfade: blends the two clips")
    u32 blend_mode = 1;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Max = 1, Tooltip = "How long the change to a new frame takes (crossfade, or the inertial ease-out)") f32 blend_time = 0.2f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How much the path ahead counts (higher: follows the input more closely)") f32 trajectory_weight = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How much the way it faces ahead counts") f32 facing_weight = 1.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How much the current feet and hips count (higher: smoother, slower to react)") f32 pose_weight = 1.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Max = 2, Tooltip = "A pick on the clip it plays, within this much of where it is, is not a jump: it plays on (fewer pops)") f32 same_clip_window = 0.25f;
    LSD_PROPERTY(Edit, Tooltip = "Left foot bone; empty: found by name (foot_l, LeftFoot...)") char left_foot[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "Right foot bone; empty: found by name") char right_foot[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "Hips bone; empty: found by name (pelvis, Hips...)") char hips[48] = "";
};

// Features per frame: left / right foot position (6), left / right foot velocity (6), hips velocity
// (3), the path's position 1/3, 2/3 and 1 s ahead (x, y: 6) and its facing then (x, y: 6).
constexpr u32 kMotionFeatures = 27;
constexpr u32 kMotionPoseFeatures = 15;  // the first ones: the pose (the rest: the path)
constexpr f32 kMotionFrameRate = 30.0f;
constexpr f32 kMotionTrajectoryTimes[3] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};

struct MotionFrame {
    u32 clip = 0;  // into MotionData::clips
    f32 time = 0.0f;
};

// A Motion Database built for one skeleton: normalised features per frame.
struct MotionData {
    explicit MotionData(Allocator& allocator) : features(allocator), frames(allocator) {}
    Array<f32> features;        // frames.size() x kMotionFeatures, normalised
    Array<MotionFrame> frames;
    f32 mean[kMotionFeatures] = {};
    f32 scale[kMotionFeatures] = {};  // (raw - mean) * scale = normalised (weights folded in)
    const Clip* clips[64] = {};
    bool looping[64] = {};
    char names[64][64] = {};
    u32 clip_first[64] = {};   // each clip's first frame in `frames`
    u32 clip_frames[64] = {};  // and how many
    u32 clip_count = 0;
    // Bake: every 1/30 s of each clip on `model` (feet, hips and the path in the character's own space:
    // x its facing, z up). `model_forward_yaw`: the way the model faces in its own space (radians; the
    // Skeletal Mesh's Facing). Clips made in place get their movement from the planted foot. False
    // (and empty) without feet / hips bones or clips.
    bool build(const SkinnedModel& model, const Clip* const* clips, const bool* looping, u32 count, const MotionDatabase& settings, f32 model_forward_yaw);
};

// What the player asks for, in the world (Z up): the velocity now and the one wanted, and the facing
// now and the one wanted (radians about Z, 0 = +X).
struct MotionInput {
    Vec3 velocity;
    Vec3 desired_velocity;
    f32 facing = 0.0f;
    f32 desired_facing = 0.0f;
};

// The path the input leads to (a critically damped spring toward the wanted velocity and facing), in
// the character's own space: positions (x, y) at kMotionTrajectoryTimes and facings (x, y) then.
void predict_trajectory(const MotionInput& input, f32 positions[6], f32 facings[6]);

// The frame whose features (already normalised) are nearest `query`; `cost` gets its distance².
u32 search_motion(const MotionData& data, const f32* query, f32* cost = nullptr);

// One object's playback: the clip and time it plays, the one it fades from, when it last searched.
struct MotionState {
    i32 clip = -1;  // -1: not started
    f32 time = 0.0f;
    i32 previous_clip = -1;
    f32 previous_time = 0.0f;
    f32 fade = 1.0f;
    f32 since_search = 1e9f;
    f32 cost = 0.0f;  // of the last pick
    bool switched = false;  // Inertial: it jumped to another frame this step (Pose::inertialize takes the offset)
};

// Advances by dt, searches when it is time, and writes what to sample (up to 2 inputs: the clip and
// the one it fades from). Returns how many.
u32 step_motion(const MotionData& data, const MotionDatabase& settings, const MotionInput& input, MotionState& state, f32 dt, BlendInput* out);

} // namespace lsd::anim
