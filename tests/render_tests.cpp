// Math and mesh-generation tests (no GPU needed).
#include "lsd/foundation/math.h"
#include "lsd/render/lod.h"
#include "lsd/render/material.h"
#include "lsd/render/mesh.h"
#include "lsd/render/occlusion.h"
#include "lsd/render/reflection.h"
#include "lsd/render/texture_streaming.h"

#include <vector>
#include "lsd/rhi/rhi.h"
#include "lsd/rhi/shader_cache.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// Projects a world point to normalised device coordinates (x, y in -1..1, depth 0..1).
static lsd::Vec3 project(const lsd::Mat4& m, lsd::Vec3 p) {
    const float w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
    const lsd::Vec3 c = lsd::transform_point(m, p);
    return {c.x / w, c.y / w, c.z / w};
}

static void test_math() {
    using namespace lsd;
    const Mat4 t = translation({1, 2, 3});
    const Vec3 moved = transform_point(t, {1, 1, 1});
    CHECK(near(moved.x, 2) && near(moved.y, 3) && near(moved.z, 4));

    const Vec3 turned = transform_point(rotation_z(1.5707963f), {1, 0, 0});  // +X yaws to +Y
    CHECK(near(turned.x, 0) && near(turned.y, 1));

    // Camera on +X looking at the origin, Z up: the target lands in the centre of the screen,
    // +Y appears on the right (right-handed, Z-up, as in Blender), +Z appears at the top.
    const Mat4 view = look_at({10, 0, 0}, {0, 0, 0}, {0, 0, 1});
    const Mat4 vp = perspective(1.0f, 1.0f, 0.1f, 100.0f) * view;
    const Vec3 centre = project(vp, {0, 0, 0});
    CHECK(near(centre.x, 0) && near(centre.y, 0));
    CHECK(project(vp, {0, 1, 0}).x > 0.0f);
    CHECK(project(vp, {0, 0, 1}).y > 0.0f);

    // Depth: near plane -> 0, far plane -> 1.
    CHECK(near(project(vp, {9.9f, 0, 0}).z, 0.0f, 1e-3f));
    CHECK(near(project(vp, {-90.0f, 0, 0}).z, 1.0f, 1e-3f));
}

static void test_meshes() {
    lsd::HeapAllocator heap("meshes");
    lsd::render::MeshData cube = lsd::render::make_cube(heap);
    CHECK(cube.vertex_count == 24 && cube.index_count == 36);
    bool on_floor = true, indices_ok = true;
    for (lsd::u32 i = 0; i < cube.vertex_count; ++i) on_floor = on_floor && cube.vertices[i].position[2] >= -1e-6f;
    for (lsd::u32 i = 0; i < cube.index_count; ++i) indices_ok = indices_ok && cube.indices[i] < cube.vertex_count;
    CHECK(on_floor && indices_ok);

    // Every cube triangle winds counter-clockwise seen from outside: (b - a) x (c - a) points along the normal.
    bool outward = true;
    for (lsd::u32 t = 0; t < cube.index_count; t += 3) {
        const auto& a = cube.vertices[cube.indices[t]];
        const auto& b = cube.vertices[cube.indices[t + 1]];
        const auto& c = cube.vertices[cube.indices[t + 2]];
        const lsd::Vec3 e1{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const lsd::Vec3 e2{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        outward = outward && lsd::dot(lsd::cross(e1, e2), {a.normal[0], a.normal[1], a.normal[2]}) > 0.0f;
    }
    CHECK(outward);

    lsd::render::MeshData sphere = lsd::render::make_sphere(heap, 16, 8);
    bool unit = true;
    for (lsd::u32 i = 0; i < sphere.vertex_count; ++i) {
        const auto& p = sphere.vertices[i].position;
        unit = unit && near(std::sqrt(p[0] * p[0] + p[1] * p[1] + (p[2] - 0.5f) * (p[2] - 0.5f)), 0.5f);
    }
    CHECK(unit);
    lsd::render::free_mesh(heap, cube);
    lsd::render::free_mesh(heap, sphere);
    CHECK(heap.allocated_bytes() == 0);
}

// GPU culling's test (the shaders run the same): a camera at the origin looking along +X keeps
// boxes ahead of it, drops those behind, to the side or past the far plane, keeps one on the edge.
static void test_culling() {
    using namespace lsd;
    const Mat4 view_proj = perspective(1.0f, 16.0f / 9.0f, 0.1f, 100.0f) * look_at({0, 0, 0}, {1, 0, 0}, {0, 0, 1});
    f32 planes[6][4];
    rhi::frustum_planes(view_proj.m, planes);
    const f32 lo[3] = {-0.5f, -0.5f, -0.5f}, hi[3] = {0.5f, 0.5f, 0.5f};
    auto at = [&](f32 x, f32 y, f32 z, f32 scale = 1.0f) {
        const f32 rows[12] = {scale, 0, 0, x, 0, scale, 0, y, 0, 0, scale, z};
        return rhi::instance_visible(planes, rows, lo, hi);
    };
    CHECK(at(10, 0, 0));
    CHECK(!at(-10, 0, 0));    // behind
    CHECK(!at(10, 40, 0));    // far to the left
    CHECK(!at(10, 0, -40));   // far below
    CHECK(!at(150, 0, 0));    // past the far plane
    CHECK(at(10, 9.0f, 0, 4.0f));  // outside by its centre, but a big box reaching into the view
    CHECK(at(0.2f, 0, 0));    // right at the camera, partly in front of the near plane
    {  // the cull pass's constants and its LOD pick (as MeshCache::pick_lod): smaller on screen, a simpler LOD
        rhi::MeshDraw draw;
        std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
        draw.index_count = 36;
        draw.instance_count = 600;
        for (int k = 0; k < 3; ++k) {
            draw.local_bounds_min[k] = -0.5f;
            draw.local_bounds_max[k] = 0.5f;
        }
        draw.lod_indices[0] = rhi::BufferHandle::make(1, 0);
        draw.lod_index_counts[0] = 18;
        draw.lod_screen_size[0] = 0.1f;
        draw.lod_indices[1] = rhi::BufferHandle::make(2, 0);
        draw.lod_index_counts[1] = 9;
        draw.lod_screen_size[1] = 0.02f;
        draw.occlusion_cull = true;
        rhi::CullConstants c;
        const u32 hiz[2] = {960, 540};
        rhi::cull_constants(draw, 3, nullptr, hiz, 0, c);  // no Hi-Z: no occlusion test
        CHECK(c.info[0] == 600 && c.info[1] == 3 && c.info[2] == (3u << 1) && c.index_counts[1] == 18 && c.index_counts[2] == 9 && c.index_counts[3] == 0);
        rhi::cull_constants(draw, 3, view_proj.m, hiz, 10, c);
        CHECK((c.info[2] & 1u) && c.box_min[3] == 960.0f && c.camera[3] == 10.0f && c.hiz_view_proj[3][2] == view_proj.at(3, 2));
        auto lod_at = [&](f32 x) {
            const f32 rows[12] = {1, 0, 0, x, 0, 1, 0, 0, 0, 0, 1, 0};
            return rhi::instance_lod(c, rows);
        };
        CHECK(lod_at(3) == 0);    // radius 0.87 at 3 m: 0.29
        CHECK(lod_at(20) == 1);   // 0.043
        CHECK(lod_at(80) == 2);   // 0.011
        draw.lod_distance_scale = 10.0f;  // View Distance: things count as 10 times nearer
        rhi::cull_constants(draw, 3, nullptr, hiz, 0, c);
        CHECK(lod_at(20) == 0);
    }
}

// Occlusion: a wall 10 m ahead (4 x 4 m) hides a small box behind it; boxes beside it, in front of it,
// bigger than it, at the camera, or with nothing added stay visible.
static void test_occlusion() {
    using namespace lsd;
    static render::OcclusionBuffer buffer;  // 128 KB: not on the stack
    const Mat4 view_proj = perspective(1.0f, 2.0f, 0.1f, 200.0f) * look_at({0, 0, 0}, {1, 0, 0}, {0, 0, 1});
    const Vec3 wall[4] = {{10, -2, -2}, {10, 2, -2}, {10, 2, 2}, {10, -2, 2}};
    const u32 quad[6] = {0, 1, 2, 0, 2, 3};
    buffer.clear();
    CHECK(buffer.box_visible(view_proj, {19, -0.5f, -0.5f}, {20, 0.5f, 0.5f}));  // nothing added yet
    buffer.add_occluder(view_proj, wall, quad, 6);
    CHECK(buffer.occluder_triangles() == 2);
    CHECK(!buffer.box_visible(view_proj, {19, -0.5f, -0.5f}, {20, 0.5f, 0.5f}));  // behind it
    CHECK(buffer.box_visible(view_proj, {19, 6.0f, -0.5f}, {20, 7.0f, 0.5f}));    // beside it
    CHECK(buffer.box_visible(view_proj, {5, -0.5f, -0.5f}, {6, 0.5f, 0.5f}));     // in front of it
    CHECK(buffer.box_visible(view_proj, {19, -6.0f, -6.0f}, {20, 6.0f, 6.0f}));   // bigger than it, sticking out
    CHECK(buffer.box_visible(view_proj, {-1, -0.5f, -0.5f}, {1, 0.5f, 0.5f}));    // around the camera
    CHECK(buffer.box_visible(view_proj, {9.5f, -0.5f, -0.5f}, {10.5f, 0.5f, 0.5f}));  // through the wall: partly in front
    // A slanted occluder is written at its farthest point: a box just behind its near edge stays visible.
    const Vec3 slope[4] = {{6, -2, -2}, {6, 2, -2}, {14, 2, 2}, {14, -2, 2}};
    buffer.clear();
    buffer.add_occluder(view_proj, slope, quad, 6);
    CHECK(buffer.box_visible(view_proj, {8, -0.2f, -0.6f}, {8.4f, 0.2f, -0.4f}));
}

// Reflection captures: each face a flat colour (its index); a tile texel looks back along its direction.
static void test_reflection() {
    using namespace lsd;
    Mat4 views[6];
    render::reflection_views({0, 0, 0}, views);
    std::vector<u8> faces[6];
    const u8* face_ptrs[6];
    for (int f = 0; f < 6; ++f) {
        faces[f].assign(render::kReflectionFace * render::kReflectionFace * 4, static_cast<u8>(40 * f + 10));
        face_ptrs[f] = faces[f].data();
    }
    std::vector<u8> atlas(render::kReflectionAtlas * render::kReflectionAtlas * 4, 0);
    render::unwrap_reflection(face_ptrs, views, 3, atlas.data());  // tile 3: column 1, row 1
    auto at = [&](f32 u, f32 v) {  // tile 3's texel at (u, v)
        const u32 x = render::kReflectionTileW + static_cast<u32>(u * render::kReflectionTileW), y = render::kReflectionTileH + static_cast<u32>(v * render::kReflectionTileH);
        return atlas[(static_cast<usize>(y) * render::kReflectionAtlas + x) * 4];
    };
    CHECK(at(0.5f, 0.5f) == 10);    // +X (longitude 0, the horizon)
    CHECK(at(0.0f, 0.5f) == 50);    // -X (longitude -pi)
    CHECK(at(0.75f, 0.5f) == 90);   // +Y
    CHECK(at(0.25f, 0.5f) == 130);  // -Y
    CHECK(at(0.5f, 0.01f) == 170);  // +Z, straight up
    CHECK(at(0.5f, 0.99f) == 210);  // -Z
    CHECK(atlas[0] == 0);           // other tiles untouched
    {  // a hard edge across +X (dark above its middle, light below) comes out smooth, not stepped: values in between
        for (u32 y = 0; y < render::kReflectionFace; ++y)
            for (u32 x = 0; x < render::kReflectionFace; ++x)
                for (int c = 0; c < 4; ++c) faces[0][(static_cast<usize>(y) * render::kReflectionFace + x) * 4 + static_cast<usize>(c)] = y < render::kReflectionFace / 2 ? 0 : 200;
        render::unwrap_reflection(face_ptrs, views, 3, atlas.data());
        u32 between = 0;
        for (u32 y = 0; y < render::kReflectionTileH; ++y) {
            const u8 v = atlas[(static_cast<usize>(render::kReflectionTileH + y) * render::kReflectionAtlas + render::kReflectionTileW + render::kReflectionTileW / 2) * 4];
            between += v > 5 && v < 195 ? 1u : 0u;
        }
        CHECK(between >= 1);
    }
    std::vector<std::vector<u8>> levels;
    std::vector<u8> flat(render::kReflectionAtlas * render::kReflectionAtlas * 4, 128);
    render::reflection_mips(flat.data(), levels);
    CHECK(levels.size() == render::kReflectionMips - 1 && levels.back().size() == 16u * 16u * 4u);
    CHECK(levels[0][0] >= 127 && levels[0][0] <= 129 && levels.back()[5] >= 127 && levels.back()[5] <= 129);  // a flat colour stays flat
    {  // an HDRI: sky 1.0 above the horizon, ground 0.25 below, into tile 2 at twice the brightness
        std::vector<u16> hdri(16 * 8 * 4);
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 16; ++x)
                for (u32 c = 0; c < 4; ++c) hdri[(y * 16 + x) * 4 + c] = y < 4 ? 0x3C00 : 0x3400;  // half 1.0 / 0.25
        std::fill(atlas.begin(), atlas.end(), u8{0});
        Vec3 averages[3];
        render::hdri_to_tile(hdri.data(), 16, 8, 2.0f, 2, atlas.data(), averages);
        auto tile2 = [&](f32 u, f32 v) {  // column 0, row 1
            const u32 x = static_cast<u32>(u * render::kReflectionTileW), y = render::kReflectionTileH + static_cast<u32>(v * render::kReflectionTileH);
            return atlas[(static_cast<usize>(y) * render::kReflectionAtlas + x) * 4];
        };
        CHECK(tile2(0.3f, 0.1f) >= 207 && tile2(0.3f, 0.1f) <= 209);  // 2.0 stored like a capture: sqrt(2 / 3)
        CHECK(tile2(0.9f, 0.9f) >= 146 && tile2(0.9f, 0.9f) <= 148);  // 0.5
        CHECK(atlas[0] == 0);                                         // tile 0 untouched
        CHECK(std::fabs(averages[0].x - 1.0f) < 1e-3f && std::fabs(averages[1].y - 1.0f) < 1e-3f && std::fabs(averages[2].z - 0.25f) < 1e-3f);
    }
    {  // the atmosphere: a blue noon overhead, a sunset redder toward the sun than away from it, a dark night
        Vec3 noon[3], sunset[3], night[3];
        render::atmosphere_to_tile({0.0f, 0.0f, 1.0f}, 1.0f, 0, atlas.data(), noon);
        auto lum = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
        CHECK(noon[0].z > noon[0].y && noon[0].y > noon[0].x);
        CHECK(lum(noon[0]) > 0.08f && lum(noon[0]) < 0.32f);  // about the old fitted zenith (0.16)
        CHECK(lum(noon[1]) > lum(noon[0]));                    // a paler, brighter horizon
        render::atmosphere_to_tile({1.0f, 0.0f, 0.03f}, 1.0f, 1, atlas.data(), sunset);
        auto tile1 = [&](f32 u, f32 v, int c) {  // column 1, row 0
            const u32 x = render::kReflectionTileW + static_cast<u32>(u * render::kReflectionTileW), y = static_cast<u32>(v * render::kReflectionTileH);
            return atlas[(static_cast<usize>(y) * render::kReflectionAtlas + x) * 4 + static_cast<usize>(c)];
        };
        CHECK(tile1(0.5f, 0.47f, 0) > tile1(0.5f, 0.47f, 2));  // toward the sun (+X), just above the horizon: red over blue
        CHECK(tile1(0.5f, 0.47f, 0) > tile1(0.0f, 0.47f, 0));  // brighter red than away from it
        render::atmosphere_to_tile({0.0f, 0.0f, -1.0f}, 1.0f, 2, atlas.data(), night);
        CHECK(lum(night[0]) < 0.02f);
    }
}

// The colour-vision filter (Unreal's r.Color.Deficiency): off or strength 0 changes nothing, white stays
// white, a deuteranope sees red and green closer, and the correction pulls them apart again.
static void test_color_vision() {
    using namespace lsd;
    auto apply = [](const f32 m[9], const f32 in[3], f32 out[3]) {
        for (int r = 0; r < 3; ++r) out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
    };
    auto gap = [](const f32 a[3], const f32 b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); };
    f32 m[9];
    rhi::color_vision_matrix(0, 1.0f, true, m);
    CHECK(near(m[0], 1) && near(m[1], 0) && near(m[4], 1) && near(m[8], 1));
    rhi::color_vision_matrix(1, 0.0f, true, m);
    CHECK(near(m[0], 1) && near(m[3], 0) && near(m[4], 1));
    const f32 white[3] = {1, 1, 1}, red[3] = {1, 0, 0}, green[3] = {0, 1, 0};
    for (u32 type = 1; type <= 3; ++type) {
        f32 sim[9], out[3];
        rhi::color_vision_matrix(type, 1.0f, false, sim);
        apply(sim, white, out);
        CHECK(near(out[0], 1, 0.02f) && near(out[1], 1, 0.02f) && near(out[2], 1, 0.02f));
    }
    f32 sim[9], fix[9], r_sim[3], g_sim[3], r_fix[3], g_fix[3], r_fix_seen[3], g_fix_seen[3];
    rhi::color_vision_matrix(1, 1.0f, false, sim);
    rhi::color_vision_matrix(1, 1.0f, true, fix);
    apply(sim, red, r_sim);
    apply(sim, green, g_sim);
    CHECK(gap(r_sim, g_sim) < gap(red, green));  // harder to tell apart
    apply(fix, red, r_fix);
    apply(fix, green, g_fix);
    apply(sim, r_fix, r_fix_seen);
    apply(sim, g_fix, g_fix_seen);
    CHECK(gap(r_fix_seen, g_fix_seen) > gap(r_sim, g_sim));  // corrected colours, as seen: further apart
    rhi::PostDraw post;
    f32 g[48] = {};
    rhi::post_color_vision(post, g);
    CHECK(g[27] == 0.0f);  // off: the composite skips it
    post.color_deficiency = 2;
    rhi::post_color_vision(post, g);
    CHECK(g[27] == 1.0f && g[19] == 0.0f);
}

// Texture streaming: what the view asks for, held to the pool, least recently drawn squeezed first.
static void test_texture_streaming() {
    using namespace lsd::render;
    CHECK(stream_size(10) == 64 && stream_size(65) == 128 && stream_size(1024) == 1024 && stream_size(kStreamWhole) == kStreamWhole);
    const lsd::u64 mb = 1024 * 1024;
    StreamTexture t[4];
    // 0: drawn close, at 1024 (4 MB), wants 4096. 1: drawn far, at 2048 (16 MB), wants 256.
    // 2: not drawn for a while, at 2048 (16 MB). 3: Mip Streaming off, at 512 and not complete.
    t[0].side = 1024; t[0].bytes = 4 * mb; t[0].wanted = 4096; t[0].last_used = 10;
    t[1].side = 2048; t[1].bytes = 16 * mb; t[1].wanted = 256; t[1].last_used = 10;
    t[2].side = 2048; t[2].bytes = 16 * mb; t[2].last_used = 3;
    t[3].side = 512; t[3].bytes = 1 * mb; t[3].streams = false; t[3].last_used = 3;
    lsd::u32 out[4];
    StreamPlan p = plan_texture_streaming(t, 4, 0, out);  // no pool: everyone gets what the view needs
    CHECK(p.resident_bytes == 37 * mb && p.squeezed == 0 && p.denied == 0);
    CHECK(out[0] == 4096 && out[1] == 256 && out[2] == 0 && out[3] == kStreamWhole && p.loads == 3);
    // A 40 MB pool: the far one shrinks first (37 -> 21.25 MB); then 4096 for the close one (64 MB) does not fit, 2048 (16 MB) does.
    p = plan_texture_streaming(t, 4, 40 * mb, out);
    CHECK(p.squeezed == 0 && out[1] == 256 && out[0] == 2048 && p.denied == 0);
    // A 20 MB pool (37 resident): the one not drawn halves first (16 -> 4 MB: 25 MB), then the largest recent one, straight
    // to the 256 it needs (9.25 MB); the close one's 2048 would make 21.25: held back.
    p = plan_texture_streaming(t, 4, 20 * mb, out);
    CHECK(p.squeezed == 2 && out[2] == 1024 && out[1] == 256 && out[3] == kStreamWhole);
    CHECK(out[0] == 0 && p.denied == 1);
    // Busy ones and the smallest are left alone; nothing to squeeze ends the loop.
    StreamTexture small[2];
    small[0].side = 64; small[0].bytes = 16 * 1024; small[0].last_used = 1;
    small[1].side = 4096; small[1].bytes = 64 * mb; small[1].busy = true; small[1].wanted = 64;
    p = plan_texture_streaming(small, 2, 1, out);
    CHECK(out[0] == 0 && out[1] == 0 && p.loads == 0);
    // Not drawn and room in the pool: kept as it is; complete ones do not get sharper.
    StreamTexture kept[2];
    kept[0].side = 512; kept[0].bytes = mb;
    kept[1].side = 512; kept[1].bytes = mb; kept[1].wanted = 2048; kept[1].complete = true;
    p = plan_texture_streaming(kept, 2, 100 * mb, out);
    CHECK(out[0] == 0 && out[1] == 0);
    // A load is all there is when smaller than asked, or asked at the texture's cap (its Max Size, little video memory).
    CHECK(stream_load_complete(512, 512, 0, 0) && stream_load_complete(256, 128, 512, 0) && !stream_load_complete(512, 512, 512, 0));
    CHECK(stream_load_complete(1024, 1024, 1024, 1024) && !stream_load_complete(512, 512, 512, 1024));
}

static void test_lods() {
    using namespace lsd::render;
    float sizes[kMaxLods];
    lod_screen_sizes(0.12f, 0.06f, 0.03f, sizes);
    CHECK(near(sizes[4], 0.015f) && near(sizes[5], 0.0075f));
    CHECK(pick_lod(sizes, 6, 0.5f, -1) == 0 && pick_lod(sizes, 6, 0.1f, -1) == 1 && pick_lod(sizes, 6, 0.001f, -1) == 5);
    CHECK(pick_lod(sizes, 3, 0.001f, -1) == 2 && pick_lod(sizes, 3, 0.5f, 7) == 2 && pick_lod(sizes, 6, 0.001f, 0) == 0);
    // A unit box (radius sqrt(3) / 2) scaled by 2, 10 m away: 0.173.
    const float rows[12] = {2, 0, 0, 10, 0, 2, 0, 0, 0, 0, 2, 0};
    const float lo[3] = {-0.5f, -0.5f, -0.5f}, hi[3] = {0.5f, 0.5f, 0.5f}, eye[3] = {0, 0, 0};
    CHECK(near(screen_size(rows, lo, hi, eye, 1.0f), 0.1732f, 1e-3f) && near(screen_size(rows, lo, hi, eye, 2.0f), 0.3464f, 1e-3f));
    // Budgets: a quarter the triangles at half the size; the allowed error doubles.
    CHECK(lod_triangle_budget(0.12f) > 9000 && lod_triangle_budget(0.12f) < 11000);
    CHECK(lod_triangle_budget(0.06f) * 4 <= lod_triangle_budget(0.12f) + 4 && lod_triangle_budget(0.0f) == 12);
    CHECK(near(lod_max_error(0.03f), 2.0f * lod_max_error(0.06f), 1e-5f));
    // Copies by LOD: near, middle, far and a speck; LOD 0 first, the speck left out.
    float copies[4 * 12];
    const float distance[4] = {1.0f, 2.0f, 40.0f, 5000.0f};
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 12; ++i) copies[k * 12 + i] = i % 5 == 0 ? 1.0f : 0.0f;
        copies[k * 12 + 3] = distance[3 - k];  // given far to near
    }
    float sorted[4 * 12];
    LodBuckets b;
    sort_by_lod(copies, 4, lo, hi, eye, 1.0f, sizes, 6, -1, 0.0015f, sorted, b);
    CHECK(b.culled == 1 && b.first[1] == 2 && b.first[kMaxLods] == 3);
    CHECK(sorted[3] == 2.0f && sorted[12 + 3] == 1.0f && sorted[24 + 3] == 40.0f);  // the two near ones kept in their order
    sort_by_lod(copies, 4, lo, hi, eye, 1.0f, sizes, 6, 2, 0.0015f, sorted, b);  // r.ForceLOD 2: all, none culled
    CHECK(b.culled == 0 && b.first[2] == 0 && b.first[3] == 4);
    // Auto LODs of a dense sphere: fewer triangles each step, LOD 1 near its budget.
    lsd::HeapAllocator heap("lods");
    MeshData sphere = make_sphere(heap, 128, 64);
    std::vector<lsd::u32> storage(static_cast<size_t>(sphere.index_count) * (kMaxLods - 1));
    lsd::u32* out[kMaxLods - 1];
    lsd::u32 counts[kMaxLods - 1] = {};
    for (lsd::u32 k = 0; k + 1 < kMaxLods; ++k) out[k] = storage.data() + static_cast<size_t>(k) * sphere.index_count;
    const lsd::u32 lods = build_lods(sphere, sizes, out, counts);
    CHECK(lods >= 4);
    lsd::u32 previous = sphere.index_count;
    for (lsd::u32 k = 1; k < lods; ++k) {
        CHECK(counts[k - 1] % 3 == 0 && counts[k - 1] < previous);
        for (lsd::u32 i = 0; i < counts[k - 1]; ++i) CHECK(out[k - 1][i] < sphere.vertex_count);
        previous = counts[k - 1];
    }
    CHECK(counts[0] <= sphere.index_count * 11 / 20);  // about half gone (or more)
    float no_far[kMaxLods];
    lod_screen_sizes(0.12f, 0.0f, 0.0f, no_far);  // LOD 2 and on never drawn: not built
    CHECK(build_lods(sphere, no_far, out, counts) == 2);
    free_mesh(heap, sphere);
}

// The shader cache: keys, hits and misses, damaged and foreign files (a temp folder, no GPU).
static void test_shader_cache() {
    using lsd::rhi::shader_cache_key;
    const lsd::u64 key = shader_cache_key("float4 main() : SV_Target { return 1; }", "main", "ps_5_0", "", "fxc");
    CHECK(key == shader_cache_key("float4 main() : SV_Target { return 1; }", "main", "ps_5_0", "", "fxc"));  // stable
    CHECK(key != shader_cache_key("float4 main() : SV_Target { return 0; }", "main", "ps_5_0", "", "fxc"));  // source
    CHECK(key != shader_cache_key("float4 main() : SV_Target { return 1; }", "main", "vs_5_0", "", "fxc"));  // target
    CHECK(key != shader_cache_key("float4 main() : SV_Target { return 1; }", "main", "ps_5_0", "SKINNED=1;", "fxc"));  // defines
    CHECK(key != shader_cache_key("float4 main() : SV_Target { return 1; }", "main", "ps_5_0", "", "dxc"));  // compiler
    CHECK(shader_cache_key("ab", "c", "", "", "") != shader_cache_key("a", "bc", "", "", ""));  // parts do not run together

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "lsd_shader_cache_test";
    fs::remove_all(dir, ec);
    const std::string folder = dir.string();
    const lsd::u8 code[] = {0x44, 0x58, 0x42, 0x43, 1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<lsd::u8> out;
    CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), key, out));  // miss: nothing stored yet
    CHECK(lsd::rhi::store_cached_shader(folder.c_str(), key, code, sizeof code));  // creates the folder
    CHECK(lsd::rhi::load_cached_shader(folder.c_str(), key, out) && out.size() == sizeof code && std::memcmp(out.data(), code, sizeof code) == 0);
    CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), key + 1, out));  // another key: no file
    fs::path file;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) file = e.path();
    CHECK(file.extension() == ".lsdshader");  // one file, no temp left behind
    {  // a file copied under another key's name is not that key's
        fs::copy_file(file, dir / "0000000000000001.lsdshader", ec);
        CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), 1, out));
    }
    const auto damage = [&](long at, int byte) {
        FILE* f = std::fopen(file.string().c_str(), "r+b");
        if (!f) return;
        std::fseek(f, at, SEEK_END);
        std::fputc(byte, f);
        std::fclose(f);
    };
    damage(-1, 0xEE);  // a flipped byte in the bytecode: the checksum catches it
    CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), key, out));
    CHECK(lsd::rhi::store_cached_shader(folder.c_str(), key, code, sizeof code));  // compiled again: replaces it
    CHECK(lsd::rhi::load_cached_shader(folder.c_str(), key, out) && out.size() == sizeof code);
    fs::resize_file(file, fs::file_size(file, ec) - 3, ec);  // cut short (a crash mid-write of an older engine, a full disk)
    CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), key, out));
    {  // longer than its header says
        CHECK(lsd::rhi::store_cached_shader(folder.c_str(), key, code, sizeof code));
        FILE* f = std::fopen(file.string().c_str(), "ab");
        if (f) {
            std::fputc(0, f);
            std::fclose(f);
        }
        CHECK(!lsd::rhi::load_cached_shader(folder.c_str(), key, out));
    }
    CHECK(!lsd::rhi::store_cached_shader(folder.c_str(), key, code, 0));  // nothing to store
    fs::remove_all(dir, ec);
}

// Material Tiling: pack_uv_scale's float decodes (as mesh.hlsl's uv_tiling does) to the scales in 1/64 steps; 1 x 1 is 0 (untiled).
static void test_uv_tiling() {
    auto decode = [](float q, float& u, float& v) { u = std::floor(q / 4096.0f) / 64.0f; v = std::fmod(q, 4096.0f) / 64.0f; };
    const float one[2] = {1.0f, 1.0f}, tiled[2] = {3.0f, 0.5f}, odd[2] = {2.3f, 63.98f}, tiny[2] = {0.0f, 100.0f};
    CHECK(lsd::rhi::pack_uv_scale(one) == 0.0f);
    float u = 0.0f, v = 0.0f;
    decode(lsd::rhi::pack_uv_scale(tiled), u, v);
    CHECK(u == 3.0f && v == 0.5f);
    decode(lsd::rhi::pack_uv_scale(odd), u, v);
    CHECK(near(u, 2.3f, 1.0f / 128.0f) && near(v, 63.98f, 1.0f / 128.0f));
    decode(lsd::rhi::pack_uv_scale(tiny), u, v);  // clamped to 1/64 .. 4095/64
    CHECK(u == 1.0f / 64.0f && v == 4095.0f / 64.0f);
    lsd::rhi::MeshDraw draw{};
    CHECK(draw.uv_scale[0] == 1.0f && draw.uv_scale[1] == 1.0f && lsd::rhi::ShadowCutout{}.uv_scale[1] == 1.0f);
    // A Material saved before Texture Setup existed loads with these: untiled, OpenGL normals, ORM as R AO / G rough / B metal.
    const lsd::render::Material m{};
    CHECK(m.tiling_u == 1.0f && m.tiling_v == 1.0f && !m.normal_directx && !m.roughness_is_gloss);
    CHECK(m.ao_channel == 0 && m.roughness_channel == 1 && m.metallic_channel == 2);
}

int main() {
    test_uv_tiling();
    test_shader_cache();
    test_color_vision();
    test_reflection();
    test_math();
    test_meshes();
    test_culling();
    test_occlusion();
    test_texture_streaming();
    test_lods();
    if (g_failures == 0) std::printf("All render tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
