// Frustum planes and the instance box test, shared by the GPU culling shaders' setup and the tests.
#include "lsd/rhi/rhi.h"

#include <cmath>
#include <cstring>

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

void cull_constants(const MeshDraw& draw, u32 batches, const f32* hiz_view_proj, const u32 hiz_size[2], u32 hiz_mips, CullConstants& out) {
    out = CullConstants{};
    frustum_planes(draw.mvp, out.planes);
    for (int k = 0; k < 3; ++k) {
        out.box_min[k] = draw.local_bounds_min[k];
        out.box_max[k] = draw.local_bounds_max[k];
        out.camera[k] = draw.camera_position[k];
    }
    u32 lods = 1;  // LOD 0, then each set one in turn
    out.index_counts[0] = draw.index_count;
    while (lods < 4 && draw.lod_indices[lods - 1].valid() && draw.lod_index_counts[lods - 1]) {
        out.index_counts[lods] = draw.lod_index_counts[lods - 1];
        out.lod[lods - 1] = draw.lod_screen_size[lods - 1];
        ++lods;
    }
    out.lod[3] = draw.lod_distance_scale > 0.0f ? draw.lod_distance_scale : 1.0f;
    const bool hiz = draw.occlusion_cull && hiz_view_proj && hiz_size && hiz_mips;
    out.info[0] = draw.instance_count;
    out.info[1] = batches;
    out.info[2] = (hiz ? 1u : 0u) | (lods << 1);
    if (hiz) {
        out.box_min[3] = static_cast<f32>(hiz_size[0]);
        out.box_max[3] = static_cast<f32>(hiz_size[1]);
        out.camera[3] = static_cast<f32>(hiz_mips);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) out.hiz_view_proj[r][c] = hiz_view_proj[c * 4 + r];
        }
    }
}

u32 instance_lod(const CullConstants& c, const f32 rows[12]) {
    const f32 cc[3] = {(c.box_min[0] + c.box_max[0]) * 0.5f, (c.box_min[1] + c.box_max[1]) * 0.5f, (c.box_min[2] + c.box_max[2]) * 0.5f};
    const f32 e[3] = {(c.box_max[0] - c.box_min[0]) * 0.5f, (c.box_max[1] - c.box_min[1]) * 0.5f, (c.box_max[2] - c.box_min[2]) * 0.5f};
    f32 wc[3], we[3];
    for (int r = 0; r < 3; ++r) {
        const f32* row = rows + r * 4;
        wc[r] = row[0] * cc[0] + row[1] * cc[1] + row[2] * cc[2] + row[3];
        we[r] = std::fabs(row[0]) * e[0] + std::fabs(row[1]) * e[1] + std::fabs(row[2]) * e[2];
    }
    const f32 radius = std::sqrt(we[0] * we[0] + we[1] * we[1] + we[2] * we[2]);
    const f32 dx = wc[0] - c.camera[0], dy = wc[1] - c.camera[1], dz = wc[2] - c.camera[2];
    const f32 size = radius * c.lod[3] / std::fmax(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
    const u32 lods = (c.info[2] >> 1) & 7u;
    u32 l = 0;
    for (u32 k = 1; k < 4; ++k) {
        if (k < lods && size < c.lod[k - 1]) l = k;
    }
    return l;
}

void morph_job(MorphJob& job, BufferHandle out, BufferHandle base, BufferHandle morphs, u32 vertex_count, const f32* weights, u32 weight_count) {
    const u32 count = weight_count < kMaxMorphTargets ? weight_count : kMaxMorphTargets;
    job.out = out;
    job.base = base;
    job.morphs = morphs;
    job.vertex_count = vertex_count;
    job.word_count = 4 + count;
    job.words[0] = vertex_count;
    job.words[1] = job.words[2] = job.words[3] = 0;
    std::memcpy(job.words + 4, weights, count * sizeof(f32));
}

} // namespace lsd::rhi
