// World tests: entities, component pools, hierarchy, level text round trip, transform math.
#include "lsd/world/components.h"
#include "lsd/world/level.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"
#include "lsd/world/curve.h"
#include "lsd/world/timeline.h"
#include "lsd/world/abilities.h"

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

        // Structural overrides: the instance adds a Light child and a Tags component and drops the
        // wheel's mesh; after the prefab changes (a new Seat child), all three are still so.
        const Entity light = w.create("Light");
        w.add<PointLight>(light)->intensity = 123.0f;
        w.set_parent(light, placed, false);
        w.add<Tags>(placed);
        w.remove(find_in_instance(w, placed, "Wheel"), reflect::type_of<StaticMesh>());
        CHECK(record_structure_overrides(w, placed, text.data(), text.size()));
        u32 structural = 0;
        for (const PrefabOverride& o : w.get<PrefabInstance>(placed)->overrides) structural += o.kind != 0;
        CHECK(structural == 3);
        const Entity seat = w.create("Seat");
        w.set_parent(seat, car, false);
        save_prefab(w, car, text);
        CHECK(update_instance(w, placed, text.data(), text.size()));
        const Entity kept_light = find_in_instance(w, placed, "Light");
        CHECK(kept_light.valid() && w.get<PointLight>(kept_light) && w.get<PointLight>(kept_light)->intensity == 123.0f);
        CHECK(w.get<Tags>(placed) && find_in_instance(w, placed, "Seat").valid() && !w.get<StaticMesh>(find_in_instance(w, placed, "Wheel")));
        // Removing a prefab child in the instance: it stays gone.
        w.destroy(find_in_instance(w, placed, "Seat"));
        CHECK(record_structure_overrides(w, placed, text.data(), text.size()));
        CHECK(update_instance(w, placed, text.data(), text.size()) && !find_in_instance(w, placed, "Seat").valid());
    }
    CHECK(heap.allocated_bytes() == 0);

    // Nested prefabs: a Room holds a Lamp instance; when the Lamp prefab gains a Shade, a newly
    // updated Room's lamp has it too (refreshed from the Lamp file through the resolver).
    {
        World w(heap);
        static const char kLamp[] = LSD_TEST_TEMP_DIR "/Lamp.lsdprefab";
        const Entity lamp = w.create("Lamp");
        w.set_parent(w.create("Bulb"), lamp, false);
        CHECK(save_prefab_file(w, lamp, kLamp));
        const Entity room = w.create("Room");
        const Entity inner = w.create("LampA");
        std::snprintf(w.add<PrefabInstance>(inner)->prefab, sizeof(PrefabInstance::prefab), "0123456789abcdef0123456789abcdef");
        CHECK(update_instance_from_file(w, inner, kLamp));
        w.set_parent(inner, room, false);
        Array<char> room_text(heap);
        save_prefab(w, room, room_text);
        w.set_parent(w.create("Shade"), lamp, false);  // the Lamp prefab changes after the Room was saved
        CHECK(save_prefab_file(w, lamp, kLamp));
        set_prefab_resolver([](void*, const char* guid, char* out, usize capacity) {
            std::snprintf(out, capacity, "%s", kLamp);
            return std::strcmp(guid, "0123456789abcdef0123456789abcdef") == 0;
        }, nullptr);
        const Entity placed = w.create("MyRoom");
        w.add<PrefabInstance>(placed);
        CHECK(update_instance(w, placed, room_text.data(), room_text.size()));
        CHECK(find_in_instance(w, placed, "LampA/Bulb").valid() && find_in_instance(w, placed, "LampA/Shade").valid());
        set_prefab_resolver(nullptr, nullptr);
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

// Sub-levels: another level's objects under a root, moved with it, not saved with this level.
static void test_sublevel() {
    using namespace lsd;
    HeapAllocator heap("sublevel");
    {
        static const char kHouse[] = LSD_TEST_TEMP_DIR "/House.lsdlevel";
        world::World house(heap);
        const world::Entity wall = house.create("Wall");
        house.get<world::Transform>(wall)->position = {1, 2, 0};
        house.add<world::StaticMesh>(wall);
        const world::Entity lamp = house.create("Lamp");
        house.set_parent(lamp, wall, false);
        house.get<world::Transform>(lamp)->position = {0, 0, 3};
        CHECK(world::save_level_file(house, kHouse));

        world::World w(heap);
        const world::Entity floor = w.create("Floor");
        const world::Entity root = w.create("HouseHere");
        w.get<world::Transform>(root)->position = {10, 0, 0};
        CHECK(!world::sublevel_loaded(w, root));
        CHECK(world::load_sublevel(w, root, kHouse));
        CHECK(world::sublevel_loaded(w, root));
        world::Entity copied_lamp;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) {
            const world::Entity e = w.entity_at_slot(slot);
            if (e.valid() && std::strcmp(w.name(e), "Lamp") == 0) copied_lamp = e;
        }
        CHECK(copied_lamp.valid() && w.locked(copied_lamp));
        const Mat4 m = copied_lamp.valid() ? w.world_matrix(copied_lamp) : Mat4{};
        CHECK(std::fabs(m.at(0, 3) - 11.0f) < 1e-4f && std::fabs(m.at(1, 3) - 2.0f) < 1e-4f && std::fabs(m.at(2, 3) - 3.0f) < 1e-4f);  // moved with the root
        Array<char> text(heap);
        world::save_level(w, text);
        text.push_back('\0');
        CHECK(std::strstr(text.data(), "HouseHere") && !std::strstr(text.data(), "\"Wall\"") && !std::strstr(text.data(), "\"Lamp\""));  // not saved here
        CHECK(world::load_sublevel(w, root, kHouse));  // again: replaces, no duplicates
        u32 walls = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) walls += w.entity_at_slot(slot).valid() && std::strcmp(w.name(w.entity_at_slot(slot)), "Wall") == 0;
        CHECK(walls == 1);
        world::unload_sublevel(w, root);
        CHECK(!world::sublevel_loaded(w, root) && w.alive(floor) && w.alive(root));
        u32 alive = 0;
        for (u32 slot = 0; slot < w.slot_count(); ++slot) alive += w.entity_at_slot(slot).valid();
        CHECK(alive == 2);
        CHECK(!world::load_sublevel(w, root, LSD_TEST_TEMP_DIR "/NoSuchLevel.lsdlevel"));
    }
    CHECK(heap.allocated_bytes() == 0);
}

// Editor Only: the object and its children go; the rest stays.
static void test_editor_only() {
    World w(default_allocator());
    const Entity note = w.create("Note");
    w.add<EditorOnly>(note);
    const Entity child = w.create("Child");
    w.set_parent(child, note);
    const Entity keep = w.create("Keep");
    CHECK(destroy_editor_only(w) == 1);
    CHECK(!w.alive(note) && !w.alive(child) && w.alive(keep));
    CHECK(destroy_editor_only(w) == 0);
}

// Ability system rules: modifiers, clamping, stacking, periodic ticks, expiry, cost / cooldown / tag checks.
static AttributeModifier modifier(const char* attribute, ModifierOp op, f32 magnitude) {
    AttributeModifier m;
    std::snprintf(m.attribute, sizeof m.attribute, "%s", attribute);
    m.op = static_cast<u32>(op);
    m.magnitude = magnitude;
    return m;
}

static void test_abilities() {
    auto close = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-3f; };
    AttributeSet set;
    const char* names[3] = {"Health", "MaxHealth", "Stamina"};
    const f32 values[3] = {100, 100, 50};
    for (u32 i = 0; i < 3; ++i) {
        Attribute a;
        std::snprintf(a.name, sizeof a.name, "%s", names[i]);
        a.base = values[i];
        set.attributes.push_back(a);
    }
    std::snprintf(find_attribute(set, "Health")->max_attribute, sizeof(Attribute::max_attribute), "MaxHealth");
    recompute(set);
    CHECK(close(find_attribute(set, "Health")->current, 100));

    GameplayEffect hit;  // Instant: the base changes, clamped at Min 0
    hit.modifiers.push_back(modifier("Health", ModifierOp::Add, -30));
    CHECK(apply_effect(set, hit, "GE_Hit") && close(find_attribute(set, "Health")->base, 70) && set.effects.count == 0);
    GameplayEffect heal;
    heal.modifiers.push_back(modifier("Health", ModifierOp::Add, 500));
    apply_effect(set, heal, "GE_Heal");
    CHECK(close(find_attribute(set, "Health")->base, 100));  // clamped to MaxHealth

    GameplayEffect buff;  // Has Duration: current only, gone when it ends
    buff.duration_type = static_cast<u32>(EffectDuration::HasDuration);
    buff.duration = 2;
    buff.modifiers.push_back(modifier("MaxHealth", ModifierOp::Multiply, 1.5f));
    buff.modifiers.push_back(modifier("Health", ModifierOp::Add, 25));
    std::snprintf(buff.granted_tags, sizeof buff.granted_tags, "Status.Buffed");
    buff.stacking = static_cast<u32>(EffectStacking::Stack);
    buff.max_stacks = 2;
    apply_effect(set, buff, "GE_Buff");
    CHECK(close(find_attribute(set, "MaxHealth")->current, 150) && close(find_attribute(set, "Health")->current, 125) && close(find_attribute(set, "Health")->base, 100));
    apply_effect(set, buff, "GE_Buff");
    apply_effect(set, buff, "GE_Buff");  // capped at 2 stacks
    CHECK(set.effects.count == 1 && set.effects[0].stacks == 2 && close(find_attribute(set, "Health")->current, 150));  // 100 + 50, MaxHealth 225
    CHECK(!tick_effects(set, 1.0f) && set.effects.count == 1);
    CHECK(tick_effects(set, 1.0f) && set.effects.count == 0 && close(find_attribute(set, "Health")->current, 100));

    GameplayEffect poison;  // periodic: 5 ticks of -10 over 5 s, the first after 1 s
    poison.duration_type = static_cast<u32>(EffectDuration::HasDuration);
    poison.duration = 5;
    poison.period = 1;
    poison.modifiers.push_back(modifier("Health", ModifierOp::Add, -10));
    poison.stacking = static_cast<u32>(EffectStacking::Refresh);
    apply_effect(set, poison, "GE_Poison");
    CHECK(close(find_attribute(set, "Health")->current, 100));
    for (int i = 0; i < 300; ++i) tick_effects(set, 1.0f / 60.0f);
    CHECK(close(find_attribute(set, "Health")->base, 50) && set.effects.count == 0);
    apply_effect(set, poison, "GE_Poison");
    tick_effects(set, 100.0f);  // one long step still ticks only within the 5 s
    CHECK(close(find_attribute(set, "Health")->base, 0) && set.effects.count == 0);

    GameplayEffect armor;  // Infinite until removed; Override wins
    armor.duration_type = static_cast<u32>(EffectDuration::Infinite);
    armor.modifiers.push_back(modifier("Stamina", ModifierOp::Override, 7));
    apply_effect(set, armor, "GE_Armor");
    apply_effect(set, armor, "GE_Armor");  // stacking None: two copies
    tick_effects(set, 1000.0f);
    CHECK(set.effects.count == 2 && close(find_attribute(set, "Stamina")->current, 7));
    CHECK(remove_effect(set, "GE_Armor") == 2 && close(find_attribute(set, "Stamina")->current, 50));

    // Abilities: cost, cooldown, tags.
    GameplayEffect cost;
    cost.modifiers.push_back(modifier("Stamina", ModifierOp::Add, -20));
    GameplayAbility dash;
    std::snprintf(dash.blocked_tags, sizeof dash.blocked_tags, "Status.Stunned");
    std::snprintf(dash.required_tags, sizeof dash.required_tags, "Hero");
    GrantedAbility state;
    CHECK(can_activate(dash, state, "Hero", "", &set, &cost) == AbilityBlock::None);
    CHECK(can_activate(dash, state, "Enemy", "", &set, &cost) == AbilityBlock::MissingTags);
    CHECK(can_activate(dash, state, "Hero, Status.Stunned", "", &set, &cost) == AbilityBlock::BlockedTags);
    CHECK(can_activate(dash, state, "Hero, Cooldown.Dash", "Cooldown.Dash", &set, &cost) == AbilityBlock::Cooldown);
    find_attribute(set, "Stamina")->base = 10;
    CHECK(can_activate(dash, state, "Hero", "", &set, &cost) == AbilityBlock::Cost && !can_afford(nullptr, cost));
    AbilitySystem abilities;
    state.cooldown_left = 1.0f;
    state.active = true;
    state.active_left = 0.5f;
    std::snprintf(state.active_tags, sizeof state.active_tags, "State.Dashing");
    abilities.abilities.push_back(state);
    CHECK(can_activate(dash, abilities.abilities[0], "Hero", "", &set, nullptr) == AbilityBlock::Active);
    u32 ended[4];
    CHECK(tick_abilities(abilities, 0.6f, ended, 4) == 1 && ended[0] == 0 && !abilities.abilities[0].active && abilities.abilities[0].active_tags[0] == 0);
    CHECK(can_activate(dash, abilities.abilities[0], "Hero", "", &set, nullptr) == AbilityBlock::Cooldown);
    tick_abilities(abilities, 0.5f, ended, 4);
    CHECK(can_activate(dash, abilities.abilities[0], "Hero", "", &set, nullptr) == AbilityBlock::None);

    HeapAllocator heap("abilities");  // granted tags count as the object's own
    {
        World w(heap);
        const Entity e = w.create("Hero");
        std::snprintf(w.add<Tags>(e)->tags, sizeof(Tags::tags), "Hero");
        AttributeSet* s = w.add<AttributeSet>(e);
        GameplayEffect stun;
        stun.duration_type = static_cast<u32>(EffectDuration::Infinite);
        std::snprintf(stun.granted_tags, sizeof stun.granted_tags, "Status.Stunned");
        apply_effect(*s, stun, "GE_Stun");
        CHECK(has_tag(w, e, "Status") && has_tag(w, e, "Hero") && has_tags(w, e, "Hero, Status.Stunned", TagQuery::All));
        remove_effect(*w.get<AttributeSet>(e), "GE_Stun");
        CHECK(!has_tag(w, e, "Status"));
    }
    CHECK(heap.allocated_bytes() == 0);
}

int main() {
    test_abilities();
    test_math();
    test_editor_only();
    test_sublevel();
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
