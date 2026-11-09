#pragma once
// Skeletal animation on ozz-animation (hidden behind this header). A SkinnedModel is a mesh bound
// to a skeleton (imported from FBX); Clips are animations sampled onto that skeleton by joint
// name, so a character and its animations can come from separate files (the Mixamo workflow);
// a Pose samples a clip and gives the matrices the GPU skins with.
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"

#include <cmath>
#include <cstring>

namespace lsd::anim {

// A morph target (Unreal's Morph Target, Blender's shape key): sparse position offsets from the
// bind pose. Several meshes of a file may each have one with the same name; they move together.
struct MorphTarget {
    char name[64] = "";
    u32 first = 0, count = 0;  // into SkinnedModel::morph_deltas
};
struct MorphDelta {
    u32 vertex = 0;
    f32 offset[3] = {0, 0, 0};
    f32 normal[3] = {0, 0, 0};  // how the vertex's normal turns at full weight (its neighbours' too, with no offset)
};

// The triangles of a skinned mesh that use one of the file's materials (Unreal's Material Slot /
// mesh section): drawn with that slot's material.
struct MeshSection {
    char material[64] = "";  // the material's name in the file ("" = triangles without a material)
    u32 first_index = 0, index_count = 0;  // into SkinnedModel::indices
};

struct SkinnedModel {
    explicit SkinnedModel(Allocator& allocator)
        : vertices(allocator), indices(allocator), sections(allocator), inverse_bind(allocator), morphs(allocator), morph_deltas(allocator) {}
    ~SkinnedModel();
    SkinnedModel(const SkinnedModel&) = delete;
    SkinnedModel& operator=(const SkinnedModel&) = delete;

    Array<rhi::SkinnedVertex> vertices;  // bind pose, model space (Z up, metres)
    Array<u32> indices;                  // grouped by section
    Array<MeshSection> sections;         // one per material, in the order the file first uses them
    Array<Mat4> inverse_bind;            // per skeleton joint
    Array<MorphTarget> morphs;
    Array<MorphDelta> morph_deltas;
    Vec3 bounds_min, bounds_max;         // bind pose
    u32 joint_count() const;
    const char* joint_name(u32 joint) const;
    i32 joint_parent(u32 joint) const;   // -1 for a root
    i32 find_joint(const char* name) const;  // -1 if there is none
    // The bone root motion comes from: Unreal's "root", else the top bone under the import's Root (Mixamo's Hips).
    i32 motion_root() const;

    struct Impl;
    Impl* impl = nullptr;  // the ozz skeleton
};

// Humanoid rigs recognised by their bone names (Unreal's skeletons; any character exported with those
// names, Character Creator's UE export too). Animations made on one play on the others: when the
// rigs or their rest poses differ, clips are retargeted (rotations copied as world-space changes from
// each rig's rest pose, missing spine / neck bones taken from their nearest match, the pelvis's
// movement scaled by the height difference), like Unreal's IK Retargeter between Mannequin and Manny.
enum class Rig : u8 { Other, UE4Mannequin, UE5Manny, UE5MannySimple };
Rig detect_rig(const char* const* bone_names, u32 count);
const char* rig_name(Rig rig);  // "UE5 Manny", "UE5 Manny (Simple)", "UE4 Mannequin", "Other"
// The bone of `source` rig that drives `bone` of `target` rig when the names differ (UE4 spine_03
// drives UE5 spine_04 / spine_05...); `bone` itself when there is no such pairing.
const char* rig_source_bone(Rig target, Rig source, const char* bone);
Rig model_rig(const SkinnedModel& model);

// Retargeting between any humanoids (Unreal's IK Retargeter chains): the part a bone plays, from its name in any
// common naming (Unreal pelvis / thigh_l, Mixamo mixamorig:LeftUpLeg, Blender DEF-thigh.L, Character Creator
// CC_Base_L_Thigh, Hips / LeftUpLeg ...). Fingers, twist bones and end markers are None.
enum class HumanBone : u8 {
    None, Pelvis, Spine, Chest, UpperChest, Neck, Head,
    ClavicleL, UpperArmL, LowerArmL, HandL, ThighL, CalfL, FootL, ToeL,
    ClavicleR, UpperArmR, LowerArmR, HandR, ThighR, CalfR, FootR, ToeR,
};
HumanBone human_bone(const char* name);  // by name alone (every spine bone is Spine)
// Every bone's part, the spine bones (parents first) spread over Spine, Chest and Upper Chest from the pelvis up.
void human_bones(const char* const* names, u32 count, HumanBone* out);
// Whether a rig has the parts retargeting needs (pelvis, head, both thighs and upper arms).
bool is_humanoid(const HumanBone* parts, u32 count);

// Reads an FBX with a skinned mesh. Meshes without a skin follow the root. False + `error` if the
// file has no skeleton or cannot be read.
// `normals` / `smoothing_angle`: as the mesh import settings (asset::MeshNormals; 1 = smooth).
bool import_skinned(const char* path, f32 scale, SkinnedModel& out, char* error, usize error_capacity, u32 normals = 1, f32 smoothing_angle = 30.0f);

// The bind pose with morph targets mixed in: `weight(name)` for each target (0 = untouched, 1 = the
// full shape; outside 0..1 exaggerates). Writes model.vertices.size() vertices to `out`, ready to skin.
// Normals turn with the shape (their offsets blended by weight, then made unit length again).
template <typename WeightOf>
void apply_morphs(const SkinnedModel& model, WeightOf weight, rhi::SkinnedVertex* out) {
    std::memcpy(out, model.vertices.data(), model.vertices.size() * sizeof(rhi::SkinnedVertex));
    bool turned = false;
    for (const MorphTarget& t : model.morphs) {
        const f32 w = weight(t.name);
        if (w == 0.0f) continue;
        for (u32 i = t.first; i < t.first + t.count; ++i) {
            const MorphDelta& d = model.morph_deltas[i];
            for (int k = 0; k < 3; ++k) {
                out[d.vertex].position[k] += d.offset[k] * w;
                out[d.vertex].normal[k] += d.normal[k] * w;
            }
            turned = turned || d.normal[0] != 0.0f || d.normal[1] != 0.0f || d.normal[2] != 0.0f;
        }
    }
    if (!turned) return;
    for (const MorphTarget& t : model.morphs) {
        if (weight(t.name) == 0.0f) continue;
        for (u32 i = t.first; i < t.first + t.count; ++i) {
            f32* n = out[model.morph_deltas[i].vertex].normal;
            const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-6f) n[0] /= len, n[1] /= len, n[2] /= len;
        }
    }
}

// A simpler version of some of a skinned mesh's triangles (an LOD): `indices` (a section of model.indices) cut to about
// Ragdoll (Unreal's Physics Asset, made from the mesh): a capsule for each bone enough of the mesh follows (fitted to
// the vertices it moves most), joined to its nearest ancestor's capsule at the bone's start. Bind pose, model space.
struct RagdollBody {
    u32 joint = 0;            // the bone it moves
    i32 parent = -1;          // the body it hangs from (an index into the bodies), -1 for the root body
    Vec3 centre;              // the capsule's centre
    Vec3 axis{0, 0, 1};       // its long axis (unit), along the bone
    f32 radius = 0.05f;
    f32 half_height = 0.0f;   // of the cylinder between the end caps (physics::BodyDesc's capsule)
    f32 mass = 1.0f;          // kg, the total shared by volume
};
constexpr u32 kMaxRagdollBodies = 32;
// Returns how many bodies were written (bones in skeleton order, parents first); 0 for a mesh without enough bones.
u32 build_ragdoll(const SkinnedModel& model, RagdollBody* out, u32 capacity, f32 total_mass = 70.0f);

// The morph targets packed for rhi::Device::morph_vertices: per vertex its first entry and entry count (2 words),
// then 8 words per entry (offset xyz, target index, normal turn xyz, 0), each vertex's entries together.
void pack_morphs(const SkinnedModel& model, Array<u32>& out);

// `ratio` of its triangles, written to `out` as indices into the same vertices (skinning and morphs unchanged). Returns
// how many; 0 when it will not get simpler. Corners are welded to simplify, then each kept vertex is one of its corners.
u32 simplify_skinned(const SkinnedModel& model, const u32* indices, u32 count, f32 ratio, u32* out);

// Import: each morph target's normal offsets, from its full shape's normals recomputed the way the mesh's are.
void add_morph_normals(SkinnedModel& model, u32 normals, f32 smoothing_angle);

struct Clip {
    Clip() = default;
    ~Clip();
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    char name[64] = "";
    f32 duration = 0.0f;  // seconds
    // A morph target's weight animated by this clip at `ratio` (0..1); false if the clip leaves it alone.
    bool morph_weight(const char* morph, f32 ratio, f32& weight) const;
    // An animation curve the file carries (an FBX user-defined number property animated in the take, by its
    // name: Unreal's imported curves) at `ratio` (0..1); false if the clip has none of that name.
    bool curve_value(const char* curve, f32 ratio, f32& value) const;
    struct Impl;
    Impl* impl = nullptr;  // the ozz animation and morph weight curves
};

// Every animation (take / stack) in an FBX, sampled at 30 frames a second onto `model`'s skeleton
// by joint name (joints the file does not animate keep their rest pose). Returns how many were
// written to `out`.
// Trim (Unreal's Crop): clip `clip` keeps only `start`..`end` seconds of its take (end 0 = to the take's end).
struct ClipTrim {
    u32 clip = 0;
    f32 start = 0.0f, end = 0.0f;
};
u32 import_clips(const char* path, const SkinnedModel& model, Clip* out, u32 capacity, const ClipTrim* trims = nullptr, u32 trim_count = 0);

// One clip's contribution to a blended pose.
struct BlendInput {
    const Clip* clip = nullptr;  // nullptr: the rest pose
    f32 ratio = 0.0f;            // 0..1 through the clip
    f32 weight = 1.0f;
    bool mirrored = false;       // motion matching: play it mirrored (Pose::mirror)
};
constexpr u32 kMaxBlendInputs = 6;  // a 2D blend space (3) crossfading from another (3)

// The morph weight a blend of clips gives `morph` (their weights mixed); false if none animates it.
bool blended_morph_weight(const BlendInput* inputs, u32 count, const char* morph, f32& weight);

// Sampling state for one animated object.
class Pose {
public:
    Pose() = default;
    ~Pose();
    Pose(const Pose&) = delete;
    Pose& operator=(const Pose&) = delete;

    // Samples `clip` at `time` seconds (wrapped when looping, clamped otherwise); no clip = rest pose.
    // Fills joint_rows(): per joint, the skinning matrix's first three rows (12 floats).
    void sample(const SkinnedModel& model, const Clip* clip, f32 time, bool looping);
    // Up to kMaxBlendInputs clips mixed by weight (normalised here), e.g. a blend space or a crossfade.
    void sample_blend(const SkinnedModel& model, const BlendInput* inputs, u32 count);
    const f32* joint_rows() const { return rows_; }
    u32 joint_count() const { return joints_; }
    Mat4 joint_model(u32 joint) const;  // the joint's model-space transform (sockets, attachments)

    // Inverse kinematics on the sampled pose (targets in model space; call after sample). The joint
    // rows are rebuilt. Two-bone (Unreal's Two Bone IK): `end_joint` reaches `target`, bending at
    // its parent the way it already bends (knee, elbow). Aim (Look At): `joint`'s `forward` axis
    // (in its own space) turns toward `target`. weight 0..1 blends with the animation.
    bool two_bone_ik(const SkinnedModel& model, u32 end_joint, Vec3 target, f32 weight);
    bool aim_ik(const SkinnedModel& model, u32 joint, Vec3 target, Vec3 forward, f32 weight);
    // A clip over the sampled pose (Unreal's Layered Blend per Bone, as montages use): from
    // `from_joint` down its children (-1: every joint), mixed in by `weight` 0..1. Call after sample.
    bool layer(const SkinnedModel& model, const Clip* clip, f32 ratio, f32 weight, i32 from_joint);
    // Root motion: the motion root's (SkinnedModel::motion_root) sideways (x, y) move away from its rest place
    // is taken out of the pose (the mesh stays over its capsule) and returned, in model space; the game moves the object.
    Vec3 strip_root(const SkinnedModel& model);
    // Inertial blending (Unreal's Inertialization): call right after sample / sample_blend. When
    // `switched` (the animation jumped this frame), the difference from last frame's pose is kept
    // and added back, halving every `half_life` seconds, so the jump eases out instead of popping.
    bool inertialize(const SkinnedModel& model, bool switched, f32 dt, f32 half_life);
    // Mirrors the sampled pose left to right (Unreal's Mirror Data Table): each joint takes its
    // partner's pose (paired by name: _l / _r, Left / Right, l_ / r_), reflected across the model's
    // plane through its forward axis (`forward_yaw` radians about Z, 0 = +X) and up. The bind pose
    // (the skinning pose) mirrors onto itself. Call after sample / sample_blend.
    bool mirror(const SkinnedModel& model, f32 forward_yaw);
    // Leader pose (Unreal's Leader Pose Component: clothes skinned to the same skeleton as a body):
    // each of `model`'s joints takes the model-space pose of `leader`'s joint with the same name
    // (from `leader_model`); joints the leader lacks stay at their bind pose. Replaces any sampled pose.
    bool follow(const SkinnedModel& model, const SkinnedModel& leader_model, const Pose& leader);
    // Moves `joint` and everything below it (-1: the whole skeleton) by `m` in model space, after the pose is
    // built: the pelvis lowered for foot placement, a foot turned to the slope. IK afterwards builds on it.
    bool transform_models(const SkinnedModel& model, i32 joint, const Mat4& m);
    // Every joint's model-space transform set as given (a ragdoll's bodies); replaces any sampled pose.
    bool set_models(const SkinnedModel& model, const Mat4* models);

private:
    bool rebuild(const SkinnedModel& model, i32 from);  // locals -> model matrices -> rows
    struct Impl;
    Impl* impl_ = nullptr;
    f32* rows_ = nullptr;
    u32 joints_ = 0;
};

} // namespace lsd::anim
