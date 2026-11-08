#pragma once
// CPU occlusion culling (Unreal's software occlusion): the nearest big meshes are drawn into a small
// depth buffer, then each object's bounding box is tested against it before it is drawn. Kept
// conservative: an occluder triangle is written at its farthest corner's distance and only on the
// pixels whose centre it covers, so occluders never look closer or larger than they are; a box that
// crosses the camera's near plane or leaves the screen counts as visible. It can miss hidden
// objects, never hide visible ones.
#include "lsd/foundation/math.h"

namespace lsd::render {

class OcclusionBuffer {
public:
    static constexpr u32 kWidth = 256, kHeight = 128;

    OcclusionBuffer() { clear(); }
    void clear();  // nothing occludes
    // A mesh's triangles (`points` in its own space, `mvp` = view-projection x its world matrix).
    // Triangles reaching behind the near plane are left out.
    void add_occluder(const Mat4& mvp, const Vec3* points, const u32* indices, u32 index_count);
    // False when the box (its own space) is fully behind what was added.
    bool box_visible(const Mat4& mvp, Vec3 box_min, Vec3 box_max) const;
    u32 occluder_triangles() const { return triangles_; }

private:
    f32 depth_[kWidth * kHeight];  // view distance (clip w) of the nearest occluder; 128 KB, keep it off the stack
    u32 triangles_ = 0;
};

} // namespace lsd::render
