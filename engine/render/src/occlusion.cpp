#include "lsd/render/occlusion.h"

#include <algorithm>
#include <cmath>

namespace lsd::render {

namespace {

constexpr f32 kNear = 0.05f;  // clip w below this: too close to the camera to project safely

struct Projected {
    f32 x, y, w;  // pixels (y down) and view distance
};

Projected project(const Mat4& m, Vec3 p) {
    const f32 x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
    const f32 y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
    const f32 w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
    const f32 inv = w > 1e-6f ? 1.0f / w : 0.0f;
    return {(x * inv * 0.5f + 0.5f) * static_cast<f32>(OcclusionBuffer::kWidth), (0.5f - y * inv * 0.5f) * static_cast<f32>(OcclusionBuffer::kHeight), w};
}

f32 edge(const Projected& a, const Projected& b, f32 x, f32 y) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); }

} // namespace

void OcclusionBuffer::clear() {
    std::fill(depth_, depth_ + kWidth * kHeight, 3.4e38f);
    triangles_ = 0;
}

void OcclusionBuffer::add_occluder(const Mat4& mvp, const Vec3* points, const u32* indices, u32 index_count) {
    for (u32 t = 0; t + 2 < index_count; t += 3) {
        const Projected a = project(mvp, points[indices[t]]), b = project(mvp, points[indices[t + 1]]), c = project(mvp, points[indices[t + 2]]);
        if (a.w < kNear || b.w < kNear || c.w < kNear) continue;
        const f32 area = edge(a, b, c.x, c.y);
        if (std::fabs(area) < 1e-6f) continue;
        const f32 far_w = std::max(a.w, std::max(b.w, c.w));  // conservative: the whole triangle as far as its farthest corner
        const i32 x0 = std::max(0, static_cast<i32>(std::floor(std::min(a.x, std::min(b.x, c.x)))));
        const i32 x1 = std::min(static_cast<i32>(kWidth) - 1, static_cast<i32>(std::ceil(std::max(a.x, std::max(b.x, c.x)))));
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(std::min(a.y, std::min(b.y, c.y)))));
        const i32 y1 = std::min(static_cast<i32>(kHeight) - 1, static_cast<i32>(std::ceil(std::max(a.y, std::max(b.y, c.y)))));
        if (x0 > x1 || y0 > y1) continue;
        ++triangles_;
        const f32 sign = area > 0.0f ? 1.0f : -1.0f;  // either winding (two-sided)
        for (i32 y = y0; y <= y1; ++y) {
            const f32 py = static_cast<f32>(y) + 0.5f;
            for (i32 x = x0; x <= x1; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f;
                if (edge(a, b, px, py) * sign < 0.0f || edge(b, c, px, py) * sign < 0.0f || edge(c, a, px, py) * sign < 0.0f) continue;
                f32& d = depth_[static_cast<usize>(y) * kWidth + static_cast<usize>(x)];
                d = std::min(d, far_w);
            }
        }
    }
}

bool OcclusionBuffer::box_visible(const Mat4& mvp, Vec3 lo, Vec3 hi) const {
    if (triangles_ == 0) return true;
    f32 x0 = 3.4e38f, y0 = 3.4e38f, x1 = -3.4e38f, y1 = -3.4e38f, near_w = 3.4e38f;
    for (int k = 0; k < 8; ++k) {
        const Projected p = project(mvp, {k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        if (p.w < kNear) return true;  // reaches the camera: cannot be judged here
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
        near_w = std::min(near_w, p.w);
    }
    // Every pixel the box touches (whole pixels, rounded outwards) must hold something nearer.
    const i32 px0 = static_cast<i32>(std::floor(x0)), px1 = static_cast<i32>(std::floor(x1));
    const i32 py0 = static_cast<i32>(std::floor(y0)), py1 = static_cast<i32>(std::floor(y1));
    if (px1 < 0 || py1 < 0 || px0 >= static_cast<i32>(kWidth) || py0 >= static_cast<i32>(kHeight)) return true;  // off screen: the frustum test decides
    if (px0 < 0 || py0 < 0 || px1 >= static_cast<i32>(kWidth) || py1 >= static_cast<i32>(kHeight)) return true;  // partly off the buffer: unknown there
    for (i32 y = py0; y <= py1; ++y) {
        for (i32 x = px0; x <= px1; ++x) {
            if (depth_[static_cast<usize>(y) * kWidth + static_cast<usize>(x)] >= near_w) return true;
        }
    }
    return false;
}

} // namespace lsd::render
