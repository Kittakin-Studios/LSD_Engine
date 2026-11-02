#pragma once
// Skeletal animation on ozz-animation (hidden behind this header). A SkinnedModel is a mesh bound
// to a skeleton (imported from FBX); Clips are animations sampled onto that skeleton by joint
// name, so a character and its animations can come from separate files (the Mixamo workflow);
// a Pose samples a clip and gives the matrices the GPU skins with.
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"

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
};

struct SkinnedModel {
    explicit SkinnedModel(Allocator& allocator)
        : vertices(allocator), indices(allocator), inverse_bind(allocator), morphs(allocator), morph_deltas(allocator) {}
    ~SkinnedModel();
    SkinnedModel(const SkinnedModel&) = delete;
    SkinnedModel& operator=(const SkinnedModel&) = delete;

    Array<rhi::SkinnedVertex> vertices;  // bind pose, model space (Z up, metres)
    Array<u32> indices;
    Array<Mat4> inverse_bind;            // per skeleton joint
    Array<MorphTarget> morphs;
    Array<MorphDelta> morph_deltas;
    Vec3 bounds_min, bounds_max;         // bind pose
    u32 joint_count() const;
    const char* joint_name(u32 joint) const;
    i32 joint_parent(u32 joint) const;   // -1 for a root
    i32 find_joint(const char* name) const;  // -1 if there is none

    struct Impl;
    Impl* impl = nullptr;  // the ozz skeleton
};

// Reads an FBX with a skinned mesh. Meshes without a skin follow the root. False + `error` if the
// file has no skeleton or cannot be read.
bool import_skinned(const char* path, f32 scale, SkinnedModel& out, char* error, usize error_capacity);

// The bind pose with morph targets mixed in: `weight(name)` for each target (0 = untouched, 1 = the
// full shape; outside 0..1 exaggerates). Writes model.vertices.size() vertices to `out`, ready to skin.
// shortcut: positions only (normals stay the bind pose's), fine for faces; add normal offsets for big shape changes
template <typename WeightOf>
void apply_morphs(const SkinnedModel& model, WeightOf weight, rhi::SkinnedVertex* out) {
    std::memcpy(out, model.vertices.data(), model.vertices.size() * sizeof(rhi::SkinnedVertex));
    for (const MorphTarget& t : model.morphs) {
        const f32 w = weight(t.name);
        if (w == 0.0f) continue;
        for (u32 i = t.first; i < t.first + t.count; ++i) {
            const MorphDelta& d = model.morph_deltas[i];
            for (int k = 0; k < 3; ++k) out[d.vertex].position[k] += d.offset[k] * w;
        }
    }
}

struct Clip {
    Clip() = default;
    ~Clip();
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    char name[64] = "";
    f32 duration = 0.0f;  // seconds
    // A morph target's weight animated by this clip at `ratio` (0..1); false if the clip leaves it alone.
    bool morph_weight(const char* morph, f32 ratio, f32& weight) const;
    struct Impl;
    Impl* impl = nullptr;  // the ozz animation and morph weight curves
};

// Every animation (take / stack) in an FBX, sampled at 30 frames a second onto `model`'s skeleton
// by joint name (joints the file does not animate keep their rest pose). Returns how many were
// written to `out`.
u32 import_clips(const char* path, const SkinnedModel& model, Clip* out, u32 capacity);

// One clip's contribution to a blended pose.
struct BlendInput {
    const Clip* clip = nullptr;  // nullptr: the rest pose
    f32 ratio = 0.0f;            // 0..1 through the clip
    f32 weight = 1.0f;
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

private:
    bool rebuild(const SkinnedModel& model, i32 from);  // locals -> model matrices -> rows
    struct Impl;
    Impl* impl_ = nullptr;
    f32* rows_ = nullptr;
    u32 joints_ = 0;
};

} // namespace lsd::anim
