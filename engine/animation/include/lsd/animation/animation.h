#pragma once
// Skeletal animation on ozz-animation (hidden behind this header). A SkinnedModel is a mesh bound
// to a skeleton (imported from FBX); Clips are animations sampled onto that skeleton by joint
// name, so a character and its animations can come from separate files (the Mixamo workflow);
// a Pose samples a clip and gives the matrices the GPU skins with.
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"

namespace lsd::anim {

struct SkinnedModel {
    explicit SkinnedModel(Allocator& allocator) : vertices(allocator), indices(allocator), inverse_bind(allocator) {}
    ~SkinnedModel();
    SkinnedModel(const SkinnedModel&) = delete;
    SkinnedModel& operator=(const SkinnedModel&) = delete;

    Array<rhi::SkinnedVertex> vertices;  // bind pose, model space (Z up, metres)
    Array<u32> indices;
    Array<Mat4> inverse_bind;            // per skeleton joint
    Vec3 bounds_min, bounds_max;         // bind pose
    u32 joint_count() const;
    const char* joint_name(u32 joint) const;
    i32 joint_parent(u32 joint) const;   // -1 for a root

    struct Impl;
    Impl* impl = nullptr;  // the ozz skeleton
};

// Reads an FBX with a skinned mesh. Meshes without a skin follow the root. False + `error` if the
// file has no skeleton or cannot be read.
bool import_skinned(const char* path, f32 scale, SkinnedModel& out, char* error, usize error_capacity);

struct Clip {
    Clip() = default;
    ~Clip();
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    char name[64] = "";
    f32 duration = 0.0f;  // seconds
    struct Impl;
    Impl* impl = nullptr;  // the ozz animation
};

// Every animation (take / stack) in an FBX, sampled at 30 frames a second onto `model`'s skeleton
// by joint name (joints the file does not animate keep their rest pose). Returns how many were
// written to `out`.
u32 import_clips(const char* path, const SkinnedModel& model, Clip* out, u32 capacity);

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
    const f32* joint_rows() const { return rows_; }
    u32 joint_count() const { return joints_; }
    Mat4 joint_model(u32 joint) const;  // the joint's model-space transform (sockets, attachments)

private:
    struct Impl;
    Impl* impl_ = nullptr;
    f32* rows_ = nullptr;
    u32 joints_ = 0;
};

} // namespace lsd::anim
