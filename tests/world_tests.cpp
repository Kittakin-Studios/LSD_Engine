// World tests: entities, component pools, hierarchy, level text round trip, transform math.
#include "lsd/world/components.h"
#include "lsd/world/level.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::world;

static bool near(Vec3 a, Vec3 b, f32 eps = 1e-4f) { return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps; }

static void test_math() {
    const Vec3 t{1, 2, 3}, r{10, -20, 30}, s{2, 1, 0.5f};
    Vec3 t2, r2, s2;
    decompose_trs(trs(t, r, s), t2, r2, s2);
    CHECK(near(t, t2) && near(r, r2, 1e-3f) && near(s, s2));
    const Mat4 m = trs(t, r, s);
    const Vec3 p = transform_point(affine_inverse(m), transform_point(m, {0.3f, -4, 7}));
    CHECK(near(p, {0.3f, -4, 7}, 1e-3f));
    CHECK(near(transform_point(rotation_euler({0, 0, 90}), {1, 0, 0}), {0, 1, 0}));  // +Z yaw turns X toward Y
}

static void test_entities() {
    HeapAllocator heap("world");
    {
        World w(heap);
        const Entity a = w.create("Floor");
        const Entity b = w.create("Box");
        CHECK(w.alive(a) && w.alive(b) && a != b);
        CHECK(w.get<Transform>(a) && w.get<Transform>(a)->scale.x == 1.0f);  // defaults copied in
        CHECK(!w.get<StaticMesh>(a));
        StaticMesh* mesh = w.add<StaticMesh>(a);
        CHECK(mesh && std::strcmp(mesh->mesh, "Cube") == 0 && w.add<StaticMesh>(a) == mesh);
        w.add<StaticMesh>(b);
        CHECK(w.count(reflect::type_of<StaticMesh>()) == 2);
        w.remove(a, reflect::type_of<StaticMesh>());
        CHECK(!w.get<StaticMesh>(a) && w.get<StaticMesh>(b) && w.owner(reflect::type_of<StaticMesh>(), 0) == b);
        w.remove(a, reflect::type_of<Transform>());
        CHECK(w.get<Transform>(a));  // Transform stays

        char name[64];
        w.unique_name("Box", name, sizeof name);
        CHECK(std::strcmp(name, "Box2") == 0);

        // Hierarchy keeps world placement; cycles are refused; destroy takes children.
        w.get<Transform>(a)->position = {10, 0, 0};
        w.get<Transform>(b)->position = {12, 0, 0};
        CHECK(w.set_parent(b, a));
        CHECK(near(w.get<Transform>(b)->position, {2, 0, 0}));
        CHECK(near(transform_point(w.world_matrix(b), {}), {12, 0, 0}));
        CHECK(!w.set_parent(a, b));
        w.destroy(a);
        CHECK(!w.alive(a) && !w.alive(b) && w.count(reflect::type_of<Transform>()) == 0);
        const Entity c = w.create("Reused");
        CHECK(c.index() == a.index() || c.index() == b.index());
        CHECK(c != a && c != b && !w.alive(a));  // generation changed: old handles stay dead
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_level_round_trip() {
    HeapAllocator heap("level");
    {
        World w(heap);
        const Entity floor = w.create("Floor \"big\"");
        w.add<StaticMesh>(floor)->color = LinearColor::hex(0x336699);
        w.get<Transform>(floor)->scale = {20, 20, 0.2f};
        const Entity start = w.create("PlayerStart");
        w.add<PlayerStart>(start)->walk_speed = 7.5f;
        w.set_parent(start, floor, false);
        w.destroy(w.create("Gap"));  // a free slot in the middle must survive too
        w.settings().kill_z = -12.5f;
        w.set_hidden(start, true);
        w.set_locked(floor, true);
        w.add<KillVolume>(floor)->enabled = false;
        const Entity last = w.create("Last");

        Array<char> text(heap), again(heap);
        save_level(w, text);
        World loaded(heap);
        CHECK(load_level(loaded, text.data(), text.size()));
        CHECK(loaded.alive(floor) && loaded.alive(start) && loaded.alive(last));  // same handles
        CHECK(std::strcmp(loaded.name(floor), "Floor \"big\"") == 0);
        CHECK(loaded.parent(start) == floor);
        CHECK(loaded.get<PlayerStart>(start) && loaded.get<PlayerStart>(start)->walk_speed == 7.5f);
        CHECK(loaded.get<StaticMesh>(floor) && std::fabs(loaded.get<StaticMesh>(floor)->color.b - 0.6f) < 1e-6f);
        CHECK(!loaded.get<StaticMesh>(start));
        CHECK(loaded.hidden(start) && !loaded.hidden(floor) && loaded.locked(floor) && !loaded.locked(start));
        CHECK(loaded.hidden_in_hierarchy(start));
        CHECK(loaded.settings().kill_z == -12.5f && loaded.get<KillVolume>(floor) && !loaded.get<KillVolume>(floor)->enabled);
        save_level(loaded, again);
        CHECK(text.size() == again.size() && std::memcmp(text.data(), again.data(), text.size()) == 0);

        const char bad[] = "{\"lsd_level\": 1, \"entities\": [{\"id\": 0, \"name\": \"X\", \"Transform\": {\"position\": oops}}]}";
        CHECK(!load_level(loaded, bad, sizeof bad - 1));
        CHECK(loaded.slot_count() == 0);
        const char newer[] = "{\"lsd_level\": 99, \"entities\": []}";
        CHECK(!load_level(loaded, newer, sizeof newer - 1));
        // Unknown components and keys are skipped (older editor, newer file or removed component).
        const char extra[] = "{\"lsd_level\": 1, \"future\": [1, 2], \"entities\": [{\"id\": 5, \"name\": \"Y\", \"Laser\": {\"power\": 3}}]}";
        CHECK(load_level(loaded, extra, sizeof extra - 1));
        Entity five;
        five.value = 5;
        CHECK(loaded.alive(five) && loaded.get<Transform>(five));
    }
    CHECK(heap.allocated_bytes() == 0);
}

int main() {
    test_math();
    test_entities();
    test_level_round_trip();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
