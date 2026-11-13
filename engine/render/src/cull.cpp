#include "lsd/render/cull.h"

#include "lsd/rhi/rhi.h"

#include <cmath>

namespace lsd::render {

void side_planes(const f32 view_proj[16], f32 planes[6][4]) {
    rhi::frustum_planes(view_proj, planes);
    for (int k = 4; k < 6; ++k) {
        planes[k][0] = planes[k][1] = planes[k][2] = 0.0f;
        planes[k][3] = 1.0f;  // everything is in front of it
    }
}

Cull classify_box(const f32 planes[6][4], const f32 lo[3], const f32 hi[3]) {
    Cull result = Cull::Inside;
    for (int k = 0; k < 6; ++k) {
        const f32* p = planes[k];
        f32 most = p[3], least = p[3];  // the corner furthest along the plane's normal, and the one furthest against it
        for (int c = 0; c < 3; ++c) {
            most += p[c] * (p[c] >= 0.0f ? hi[c] : lo[c]);
            least += p[c] * (p[c] >= 0.0f ? lo[c] : hi[c]);
        }
        if (most < 0.0f) return Cull::Outside;
        if (least < 0.0f) result = Cull::Partly;
    }
    return result;
}

namespace {
// A copy's box centre and half size in the world (centre through the rows, half size through their absolute value).
void world_box(const f32 rows[12], const f32 lo[3], const f32 hi[3], f32 centre[3], f32 half[3]) {
    for (int r = 0; r < 3; ++r) {
        const f32* row = rows + r * 4;
        centre[r] = row[3];
        half[r] = 0.0f;
        for (int c = 0; c < 3; ++c) {
            centre[r] += row[c] * (lo[c] + hi[c]) * 0.5f;
            half[r] += std::fabs(row[c]) * (hi[c] - lo[c]) * 0.5f;
        }
    }
}
} // namespace

void grow_box(const f32 rows[12], const f32 lo[3], const f32 hi[3], f32 out_lo[3], f32 out_hi[3]) {
    f32 centre[3], half[3];
    world_box(rows, lo, hi, centre, half);
    for (int r = 0; r < 3; ++r) {
        out_lo[r] = std::fmin(out_lo[r], centre[r] - half[r]);
        out_hi[r] = std::fmax(out_hi[r], centre[r] + half[r]);
    }
}

void copy_sphere(const f32 rows[12], const f32 lo[3], const f32 hi[3], f32 centre[3], f32& radius) {
    f32 half[3];
    world_box(rows, lo, hi, centre, half);
    radius = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
}

} // namespace lsd::render
