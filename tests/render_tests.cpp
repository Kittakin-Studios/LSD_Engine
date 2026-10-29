// Math and mesh-generation tests (no GPU needed).
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"

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

int main() {
    test_math();
    test_meshes();
    if (g_failures == 0) std::printf("All render tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
