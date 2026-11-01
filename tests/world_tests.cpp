// World tests: entities, component pools, hierarchy, level text round trip, transform math.
#include "lsd/world/components.h"
#include "lsd/world/level.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"
#include "lsd/world/curve.h"
#include "lsd/world/timeline.h"

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
        w.set_folder(floor, "Blockout/Ground");
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
        CHECK(std::strcmp(loaded.folder(floor), "Blockout/Ground") == 0 && loaded.folder(start)[0] == '\0');
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

static void test_prefab() {
    HeapAllocator heap("prefab");
    {
        World w(heap);
        const Entity car = w.create("Car");
        w.get<Transform>(car)->position = {5, 6, 7};
        w.get<Transform>(car)->scale = {2, 2, 2};
        w.add<StaticMesh>(car)->color = LinearColor::hex(0xFF0000);
        const Entity wheel = w.create("Wheel");
        w.get<Transform>(wheel)->position = {1, 0, 0};
        w.set_parent(wheel, car, false);
        Array<char> text(heap);
        save_prefab(w, car, text);

        // A placed instance: its own placement and name stay, the rest comes from the prefab.
        const Entity placed = w.create("MyCar");
        w.get<Transform>(placed)->position = {-3, 0, 0};
        w.add<PrefabInstance>(placed);
        w.add<KillVolume>(placed);  // not in the prefab: removed
        CHECK(update_instance(w, placed, text.data(), text.size()));
        CHECK(w.get<Transform>(placed)->position.x == -3.0f && std::strcmp(w.name(placed), "MyCar") == 0);
        CHECK(w.get<Transform>(placed)->scale.x == 1.0f);  // an existing instance keeps its placement...
        const Entity fresh = w.create("Fresh");
        w.get<Transform>(fresh)->position = {9, 0, 0};
        CHECK(update_instance(w, fresh, text.data(), text.size(), true));
        CHECK(w.get<Transform>(fresh)->scale.x == 2.0f && w.get<Transform>(fresh)->position.x == 9.0f);  // ...a new one takes the prefab's scale
        CHECK(w.get<StaticMesh>(placed) && w.get<PrefabInstance>(placed) && !w.get<KillVolume>(placed));
        u32 children = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const Entity e = w.entity_at_slot(slot);
            if (e.valid() && w.parent(e) == placed) {
                ++children;
                CHECK(w.get<Transform>(e)->position.x == 1.0f);
            }
        }
        CHECK(children == 1);
        // Updating again replaces the children instead of adding more.
        CHECK(update_instance(w, placed, text.data(), text.size()));
        children = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) children += w.entity_at_slot(slot).valid() && w.parent(w.entity_at_slot(slot)) == placed;
        CHECK(children == 1);
        // Overrides: the instance's wheel colour survives the prefab changing; the rest follows.
        Entity wheel_copy;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const Entity e = w.entity_at_slot(slot);
            if (e.valid() && w.parent(e) == placed) wheel_copy = e;
        }
        char path[64];
        CHECK(instance_path(w, placed, wheel_copy, path, sizeof path) && std::strcmp(path, "Wheel") == 0);  // prefab names, not "Wheel2"
        CHECK(instance_path(w, placed, placed, path, sizeof path) && path[0] == '\0');
        CHECK(!instance_path(w, placed, car, path, sizeof path));
        CHECK(find_in_instance(w, placed, "Wheel") == wheel_copy && !find_in_instance(w, placed, "Nope").valid());
        w.add<StaticMesh>(wheel_copy)->color = LinearColor::hex(0x00FF00);
        w.get<Transform>(wheel_copy)->position.y = 7.0f;
        CHECK(set_override(w, placed, "Wheel", "StaticMesh", "color", true) && is_overridden(w, placed, "Wheel", "StaticMesh", "color"));
        w.add<StaticMesh>(wheel)->color = LinearColor::hex(0x0000FF);  // the prefab's wheel gets a mesh too and moves
        w.get<Transform>(wheel)->position.x = 3.0f;
        save_prefab(w, car, text);
        CHECK(update_instance(w, placed, text.data(), text.size()));
        const Entity new_wheel = find_in_instance(w, placed, "Wheel");
        CHECK(new_wheel.valid() && w.get<StaticMesh>(new_wheel));
        CHECK(w.get<StaticMesh>(new_wheel)->color.g == 1.0f && w.get<StaticMesh>(new_wheel)->color.b == 0.0f);  // kept
        CHECK(w.get<Transform>(new_wheel)->position.x == 3.0f && w.get<Transform>(new_wheel)->position.y == 0.0f);  // not overridden: follows
        CHECK(set_override(w, placed, "Wheel", "StaticMesh", "color", false) && !is_overridden(w, placed, "Wheel", "StaticMesh", "color"));
        CHECK(update_instance(w, placed, text.data(), text.size()));
        CHECK(w.get<StaticMesh>(find_in_instance(w, placed, "Wheel"))->color.b == 1.0f);  // reverted

        const char empty[] = "{\"lsd_level\": 1, \"entities\": []}";
        CHECK(!update_instance(w, placed, empty, sizeof empty - 1) && w.get<StaticMesh>(placed));
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_tags() {
    CHECK(tags_match("Enemy, Damage.Fire", "Damage"));
    CHECK(tags_match("Enemy, Damage.Fire", "Damage.Fire"));
    CHECK(tags_match("Enemy , Damage.Fire", "Enemy"));
    CHECK(!tags_match("Enemy, Damage.Fire", "Fire"));
    CHECK(!tags_match("Enemy, Damage.Fire", "Dam"));
    CHECK(!tags_match("Enemy, Damage.Fire", "Damage.Fire.Big"));
    CHECK(!tags_match("", "Enemy") && !tags_match("Enemy", ""));
    CHECK(tags_query("Enemy, Damage.Fire", "Boss, Damage", TagQuery::Any));
    CHECK(!tags_query("Enemy, Damage.Fire", "Boss, Flying", TagQuery::Any));
    CHECK(tags_query("Enemy, Damage.Fire", "Enemy , Damage.Fire", TagQuery::All));
    CHECK(!tags_query("Enemy, Damage.Fire", "Enemy, Boss", TagQuery::All));
    CHECK(!tags_query("Enemy", "", TagQuery::All));  // nothing asked for
    CHECK(tags_query("Enemy", "Boss, Friendly", TagQuery::None) && !tags_query("Enemy", "Boss, Enemy", TagQuery::None));
    HeapAllocator heap("tags");
    {
        World w(heap);
        const Entity e = w.create("Orc");
        CHECK(!has_tag(w, e, "Enemy"));
        std::snprintf(w.add<Tags>(e)->tags, sizeof(Tags::tags), "Enemy.Orc");
        CHECK(has_tag(w, e, "Enemy") && !has_tag(w, e, "Orc"));
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_spline() {
    const Vec3 line[3] = {{0, 0, 0}, {5, 0, 0}, {10, 0, 0}};
    CHECK(spline_segments(3, false) == 2 && spline_segments(3, true) == 3 && spline_segments(1, false) == 0);
    CHECK(length(spline_point(line, 3, false, 1.0f) - line[1]) < 1e-5f);  // passes through its points
    CHECK(std::fabs(spline_length(line, 3, false) - 10.0f) < 0.01f);
    Vec3 tangent;
    const Vec3 mid = spline_at_distance(line, 3, false, 2.5f, &tangent);
    CHECK(std::fabs(mid.x - 2.5f) < 0.05f && tangent.x > 0.0f && std::fabs(tangent.y) < 1e-5f);
    CHECK(length(spline_at_distance(line, 3, false, 99.0f) - line[2]) < 1e-4f);  // clamped to the end
    CHECK(length(spline_at_distance(line, 3, false, -1.0f) - line[0]) < 1e-4f);
    // A closed square: every corner on the curve, longer than its 4 m perimeter? No: Catmull-Rom bulges out a little.
    const Vec3 square[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    CHECK(length(spline_point(square, 4, true, 3.0f) - square[3]) < 1e-5f);
    CHECK(length(spline_point(square, 4, true, 4.0f) - square[0]) < 1e-5f);  // closes the loop
    const f32 around = spline_length(square, 4, true);
    CHECK(around > 3.9f && around < 4.6f);
    Spline s;
    CHECK(s.points.count == 2 && s.points[1].x == 4.0f);  // a new Spline starts as a 4 m line

    // Tangent handles: a sideways tangent at the start bends the line, the ends stay put.
    const Vec3 bent[2] = {{0, 4, 0}, {}};
    CHECK(length(spline_point(line, 3, false, 0.0f, bent) - line[0]) < 1e-5f);
    CHECK(spline_point(line, 3, false, 0.5f, bent).y > 0.3f);
    CHECK(std::fabs(spline_point(line, 3, false, 0.5f).y) < 1e-5f);  // no tangents: straight as before

    // Spline Meshes: copies every 2 m along a straight 4 m spline, facing along it.
    HeapAllocator heap("spline");
    World w(heap);
    const Entity fence = w.create("Fence");
    w.add<Spline>(fence);
    w.add<SplineMeshes>(fence)->spacing = 2.0f;
    update_spline_meshes(w);
    const auto* copies = w.get<InstancedStaticMesh>(fence);
    CHECK(copies && copies->instances.count == 3);
    CHECK(copies && std::fabs(copies->instances[1].position.x - 2.0f) < 0.05f && std::fabs(copies->instances[2].position.x - 4.0f) < 0.05f);
    w.get<SplineMeshes>(fence)->spacing = 1.0f;  // a change rebuilds them
    update_spline_meshes(w);
    CHECK(w.get<InstancedStaticMesh>(fence)->instances.count == 5);
}

static void test_curve() {
    Curve c;  // default: 0 at 0 s to 1 at 1 s, smooth
    CHECK(evaluate(c, -1.0f) == 0.0f && evaluate(c, 5.0f) == 1.0f);  // flat outside the keys
    CHECK(std::fabs(evaluate(c, 0.5f) - 0.5f) < 1e-5f);              // symmetric ease
    CHECK(evaluate(c, 0.25f) < 0.25f);                               // eases in (flat end tangents)
    c.keys[0].interp = 1;  // linear
    CHECK(std::fabs(evaluate(c, 0.25f) - 0.25f) < 1e-5f);
    c.keys[0].interp = 2;  // constant
    CHECK(evaluate(c, 0.99f) == 0.0f);
    // Unsorted keys after editing; a middle key keeps the curve through it.
    c.keys.push_back({0.5f, 2.0f, 0});
    c.keys[0] = {2.0f, 0.0f, 0};
    sort_keys(c);
    CHECK(c.keys[0].time == 0.5f && c.keys[1].time == 1.0f && c.keys[2].time == 2.0f);
    CHECK(evaluate(c, 1.0f) == 1.0f && evaluate(c, 0.5f) == 2.0f);
    const f32 between = evaluate(c, 1.5f);
    CHECK(between < 1.0f && between > 0.0f);

    // A User tangent: a steep start makes the curve rise early (an overshoot the auto ends never give).
    Curve steep;
    steep.keys[0].tangent_mode = 1;
    steep.keys[0].tangent = 4.0f;
    CHECK(evaluate(steep, 0.25f) > 0.4f && evaluate(steep, 0.5f) > 0.6f && evaluate(Curve{}, 0.25f) < 0.25f);
}

static void test_timeline() {  // Sequencer basics: eased transforms, values, keys crossed in order
    world::Timeline t;
    world::TimelineTrack move;
    t.tracks.push_back(move);
    world::TimelineTrack events;
    events.type = static_cast<u32>(world::TrackType::Event);
    t.tracks.push_back(events);
    world::TimelineKey k;
    k.track = 0;
    k.time = 1.0f;
    k.position = {0, 0, 0};
    k.value = 10.0f;
    t.keys.push_back(k);
    k.time = 3.0f;
    k.position = {4, 0, 2};
    k.rotation = {0, 0, 90};
    k.value = 20.0f;
    t.keys.push_back(k);
    for (f32 time : {2.5f, 1.5f}) {  // events out of order in the list
        world::TimelineKey e;
        e.track = 1;
        e.time = time;
        std::snprintf(e.text, sizeof e.text, time < 2.0f ? "first" : "second");
        t.keys.push_back(e);
    }
    Vec3 p, r;
    CHECK(world::sample_transform(t, 0, 0.0f, p, r) && p.x == 0.0f);  // held before the first key
    CHECK(world::sample_transform(t, 0, 2.0f, p, r) && std::fabs(p.x - 2.0f) < 1e-4f && std::fabs(r.z - 45.0f) < 1e-3f);  // half-way
    CHECK(world::sample_transform(t, 0, 1.5f, p, r) && p.x > 0.0f && p.x < 1.0f);  // eased: slower near the key than linear (1.0)
    CHECK(world::sample_transform(t, 0, 9.0f, p, r) && p.z == 2.0f);  // held after the last
    f32 v = 0;
    CHECK(world::sample_value(t, 0, 2.0f, v) && std::fabs(v - 15.0f) < 1e-4f);
    CHECK(!world::sample_transform(t, 5, 1.0f, p, r));
    const world::TimelineKey* fired[4];
    CHECK(world::keys_crossed(t, 1, 0.0f, 3.0f, fired, 4) == 2 && std::strcmp(fired[0]->text, "first") == 0);
    CHECK(world::keys_crossed(t, 1, 1.5f, 2.0f, fired, 4) == 0);  // exactly at `from` does not fire again
}

int main() {
    test_math();
    test_entities();
    test_level_round_trip();
    test_prefab();
    test_tags();
    test_spline();
    test_curve();
    test_timeline();
    if (g_failures == 0) std::printf("All world tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
