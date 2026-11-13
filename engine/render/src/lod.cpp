#include "lsd/render/lod.h"

#include <meshoptimizer.h>

#include <cmath>
#include <cstring>

namespace lsd::render {

namespace {

constexpr f32 kPixelsPerTriangle = 4.0f;

constexpr u32 kNoLod = 0xFFFFFFFFu;

u32 copy_lod(const f32 rows[12], const f32 lo[3], const f32 hi[3], const f32 eye[3], f32 distance_scale, const f32 sizes[kMaxLods], u32 lod_count,
             i32 force, f32 min_size) {
    const f32 size = screen_size(rows, lo, hi, eye, distance_scale);
    if (force < 0 && size < min_size) return kNoLod;
    return pick_lod(sizes, lod_count, size, force);
}

} // namespace

void lod_screen_sizes(f32 lod1, f32 lod2, f32 lod3, f32 sizes[kMaxLods]) {
    sizes[0] = 1e30f;
    sizes[1] = lod1;
    sizes[2] = lod2;
    sizes[3] = lod3;
    sizes[4] = lod3 * 0.5f;
    sizes[5] = lod3 * 0.25f;
}

f32 screen_size(const f32 rows[12], const f32 lo[3], const f32 hi[3], const f32 eye[3], f32 distance_scale) {
    // The box's centre through the matrix, its half size through the matrix's absolute value (as the GPU cull pass).
    f32 radius2 = 0.0f, distance2 = 0.0f;
    for (int r = 0; r < 3; ++r) {
        const f32* row = rows + r * 4;
        f32 centre = row[3], extent = 0.0f;
        for (int c = 0; c < 3; ++c) {
            centre += row[c] * (lo[c] + hi[c]) * 0.5f;
            extent += std::fabs(row[c]) * (hi[c] - lo[c]) * 0.5f;
        }
        radius2 += extent * extent;
        distance2 += (centre - eye[r]) * (centre - eye[r]);
    }
    return std::sqrt(radius2) * distance_scale / std::fmax(std::sqrt(distance2), 1e-3f);
}

u32 pick_lod(const f32 sizes[kMaxLods], u32 lod_count, f32 size, i32 force) {
    if (lod_count == 0) return 0;
    if (force >= 0) return static_cast<u32>(force) < lod_count ? static_cast<u32>(force) : lod_count - 1;
    u32 lod = 0;
    while (lod + 1 < lod_count && size < sizes[lod + 1]) ++lod;
    return lod;
}

u32 shadow_lod(const f32 sizes[kMaxLods], u32 lod_count, u32 camera_lod, f32 radius, f32 texel) {
    if (!(texel > 0.0f) || lod_count <= 1) return camera_lod;
    const u32 lod = pick_lod(sizes, lod_count, radius / texel / kPixelsPerRadian, -1);
    return lod > camera_lod ? lod : camera_lod;
}

u32 lod_triangle_budget(f32 screen_size) {
    const f32 pixels = 3.14159265f * (screen_size * kPixelsPerRadian) * (screen_size * kPixelsPerRadian);  // the disc it covers
    return static_cast<u32>(std::fmax(pixels / kPixelsPerTriangle, 12.0f));
}

f32 lod_max_error(f32 screen_size) {
    // meshoptimizer's error is relative to the mesh's extent, up to twice the bounding radius screen_size measures.
    return 0.5f / std::fmax(screen_size * kPixelsPerRadian, 1.0f);
}

u32 build_lods(const MeshData& mesh, const f32 sizes[kMaxLods], u32* const out[kMaxLods - 1], u32 counts[kMaxLods - 1], bool lock_border) {
    constexpr u32 kMinTriangles = 64;      // a box or a quad: nothing to gain
    constexpr f32 kHalvingError = 0.05f;   // the old rule's limit: a distant LOD may drift this far (of the mesh's size)
    if (mesh.index_count / 3 < kMinTriangles || !mesh.vertex_count) return 1;
    // Each LOD from the mesh itself (not the one before), so errors do not add up.
    const unsigned options = lock_border ? meshopt_SimplifyLockBorder : 0u;
    u32 previous_count = mesh.index_count;
    u32 lods = 1;
    for (u32 k = 1; k < kMaxLods; ++k) {
        if (!(sizes[k] > 0.0f) || previous_count / 3 <= 4) break;  // never drawn, or nothing left to simplify
        u32* dst = out[k - 1];
        const usize half = previous_count / 2 / 3 * 3;
        const usize budget = static_cast<usize>(lod_triangle_budget(sizes[k])) * 3;
        auto simplify = [&](usize target, f32 error) {
            return static_cast<u32>(meshopt_simplify(dst, mesh.indices, mesh.index_count, mesh.vertices[0].position, mesh.vertex_count, sizeof(rhi::MeshVertex), target, error,
                                                     options, nullptr));
        };
        u32 count = budget < half ? simplify(budget, lod_max_error(sizes[k])) : 0u;  // as simple as the screen allows
        if (count == 0 || count > half) count = simplify(half, kHalvingError);     // else half the last one
        if ((count == 0 || count > previous_count * 3 / 4) && k >= 2 && !lock_border) {  // stuck (separate pieces, borders): cluster
            const usize target = budget < half ? budget : half;
            const u32 sloppy = static_cast<u32>(meshopt_simplifySloppy(dst, mesh.indices, mesh.index_count, mesh.vertices[0].position, mesh.vertex_count, sizeof(rhi::MeshVertex),
                                                                     target, lod_max_error(sizes[k]) * 2.0f, nullptr));
            if (sloppy > 0) count = sloppy;
        }
        if (count == 0 || count > previous_count * 3 / 4) break;  // the shape will not get simpler: stop
        counts[k - 1] = count;
        previous_count = count;
        lods = k + 1;
    }
    return lods;
}

void sort_by_lod(const f32* rows, u32 count, const f32 lo[3], const f32 hi[3], const f32 eye[3], f32 distance_scale, const f32 sizes[kMaxLods], u32 lod_count, i32 force,
                 f32 min_size, f32* out_rows, LodBuckets& out) {
    out = LodBuckets{};
    u32 per_lod[kMaxLods] = {};
    constexpr u32 kKept = 1024;  // an Instanced Static Mesh's most copies: their LODs are kept for the second pass
    u8 picked[kKept];
    for (u32 i = 0; i < count; ++i) {  // first how many each LOD gets, then each copy into its place
        const u32 lod = copy_lod(rows + i * 12, lo, hi, eye, distance_scale, sizes, lod_count, force, min_size);
        if (i < kKept) picked[i] = static_cast<u8>(lod == kNoLod ? 0xFF : lod);
        if (lod == kNoLod) ++out.culled;
        else ++per_lod[lod];
    }
    for (u32 k = 0; k < kMaxLods; ++k) out.first[k + 1] = out.first[k] + per_lod[k];
    u32 next[kMaxLods];
    std::memcpy(next, out.first, sizeof next);
    for (u32 i = 0; i < count; ++i) {
        const u32 lod = i < kKept ? (picked[i] == 0xFF ? kNoLod : picked[i]) : copy_lod(rows + i * 12, lo, hi, eye, distance_scale, sizes, lod_count, force, min_size);
        if (lod != kNoLod) std::memcpy(out_rows + static_cast<usize>(next[lod]++) * 12, rows + static_cast<usize>(i) * 12, 12 * sizeof(f32));
    }
}

} // namespace lsd::render
