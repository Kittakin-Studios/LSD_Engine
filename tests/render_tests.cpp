// Math and mesh-generation tests (no GPU needed).
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"
#include "lsd/render/occlusion.h"
#include "lsd/render/reflection.h"

#include <vector>
#include "lsd/rhi/rhi.h"

#include <cmath>
#include <cstdio>

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
    std::vector<std::vector<u8>> levels;
    std::vector<u8> flat(render::kReflectionAtlas * render::kReflectionAtlas * 4, 128);
    render::reflection_mips(flat.data(), levels);
    CHECK(levels.size() == render::kReflectionMips - 1 && levels.back().size() == 16u * 16u * 4u);
    CHECK(levels[0][0] >= 127 && levels[0][0] <= 129 && levels.back()[5] >= 127 && levels.back()[5] <= 129);  // a flat colour stays flat
}

int main() {
    test_reflection();
    test_math();
    test_meshes();
    test_culling();
    test_occlusion();
    if (g_failures == 0) std::printf("All render tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
