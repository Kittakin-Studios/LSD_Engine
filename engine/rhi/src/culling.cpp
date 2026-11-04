// Frustum planes and the instance box test, shared by the GPU culling shaders' setup and the tests.
#include "lsd/rhi/rhi.h"

#include <cmath>

namespace lsd::rhi {

void frustum_planes(const f32 m[16], f32 planes[6][4]) {
    // Row r of the column-major matrix: m[r], m[4 + r], m[8 + r], m[12 + r].
    auto row = [&](int r, int c) { return m[c * 4 + r]; };
    const int sign[6] = {1, -1, 1, -1, 1, -1};
    const int axis[6] = {0, 0, 1, 1, 2, 2};
    for (int p = 0; p < 6; ++p) {
        for (int c = 0; c < 4; ++c) {
            // left / right / bottom / top: w +- x, w +- y; near: w + z (GL's; looser than D3D's z), far: w - z
            planes[p][c] = row(3, c) + static_cast<f32>(sign[p]) * row(axis[p], c);
        }
        const f32 len = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
        if (len > 0.0f) {
            for (f32& v : planes[p]) v /= len;
        }
    }
}

bool instance_visible(const f32 planes[6][4], const f32 rows[12], const f32 lo[3], const f32 hi[3]) {
    // The box's centre and half size in the world: centre through the matrix, half size through its absolute value.
    const f32 c[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const f32 e[3] = {(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    f32 wc[3], we[3];
    for (int r = 0; r < 3; ++r) {
        const f32* row = rows + r * 4;
        wc[r] = row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3];
        we[r] = std::fabs(row[0]) * e[0] + std::fabs(row[1]) * e[1] + std::fabs(row[2]) * e[2];
    }
    for (int p = 0; p < 6; ++p) {
        const f32 distance = planes[p][0] * wc[0] + planes[p][1] * wc[1] + planes[p][2] * wc[2] + planes[p][3];
        const f32 radius = std::fabs(planes[p][0]) * we[0] + std::fabs(planes[p][1]) * we[1] + std::fabs(planes[p][2]) * we[2];
        if (distance < -radius) return false;
    }
    return true;
}

} // namespace lsd::rhi
