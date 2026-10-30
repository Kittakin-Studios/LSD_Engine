// Script tests: a Lua file moves its object, finds others, reads tags and input, survives errors
// and reloads when the file changes.
#include "lsd/script/graph.h"
#include "lsd/script/script.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::world;

static const char kMover[] = LSD_TEST_TEMP_DIR "/mover.lua";
static const char kBroken[] = LSD_TEST_TEMP_DIR "/broken.lua";
static const char kGraphLua[] = LSD_TEST_TEMP_DIR "/graph.lua";

static void write(const char* path, const char* text) {
    FILE* f = std::fopen(path, "wb");
    std::fputs(text, f);
    std::fclose(f);
}

static bool path_of(void*, const char* guid, char* out, usize capacity) {
    std::snprintf(out, capacity, "%s", std::strcmp(guid, "mover") == 0 ? kMover : std::strcmp(guid, "graph") == 0 ? kGraphLua : kBroken);
    return true;
}

static Vec3 input(void*, const char* action) { return std::strcmp(action, "IA_Move") == 0 ? Vec3{0.5f, 1.0f, 0} : Vec3{}; }
static bool pressed(void*, const char* action) { return std::strcmp(action, "IA_Jump") == 0; }

static World* g_world = nullptr;
static Entity spawn(void*, const char* name, Vec3 position) {
    if (std::strcmp(name, "Mover") != 0) return {};
    const Entity e = g_world->create("Spawned");
    g_world->get<Transform>(e)->position = position;
    std::snprintf(g_world->add<Script>(e)->script, sizeof(Script::script), "mover");
    return e;
}
static bool raycast(void*, Vec3 origin, Vec3, f32, Entity& hit, Vec3& point, Vec3& normal) {
    hit = g_world->entity_at_slot(0);
    point = origin;
    normal = {0, 0, 1};
    return origin.z > 0;
}

// A visual script: Start moves the object; Update spins it while it is tagged Enemy, else prints its name.
static void test_graph() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 set = g.add("SetPosition", 200, 0)->id;
    const u32 make = g.add("MakeVector", 0, 100)->id;
    std::snprintf(g.find(make)->values[0], 64, "1");
    std::snprintf(g.find(make)->values[1], 64, "2.5");
    std::snprintf(g.find(make)->values[2], 64, "oops");  // not a number: 0
    CHECK(g.connect(start, 0, set, 0));
    CHECK(g.connect(make, 0, set, 2));
    CHECK(!g.connect(make, 0, set, 1));  // a Vector is not an object
    CHECK(!g.connect(start, 0, set, 2)); // exec into data

    const u32 update = g.add("OnUpdate", 0, 300)->id;
    const u32 branch = g.add("Branch", 200, 300)->id;
    const u32 tag = g.add("HasTag", 0, 400)->id;
    const u32 spin = g.add("AddRotation", 400, 300)->id;
    const u32 scale = g.add("VectorScale", 200, 450)->id;
    const u32 print = g.add("PrintString", 400, 450)->id;
    const u32 name = g.add("GetName", 200, 550)->id;
    CHECK(g.connect(update, 0, branch, 0) && g.connect(tag, 0, branch, 1));
    CHECK(g.connect(branch, 0, spin, 0) && g.connect(branch, 1, print, 0));
    std::snprintf(g.find(scale)->values[0], 64, "0, 0, 90");
    CHECK(g.connect(update, 1, scale, 1) && g.connect(scale, 0, spin, 2));  // 90 deg/s * dt
    CHECK(g.connect(name, 0, print, 1));
    const u32 every = g.add("SetTimer", 600, 0)->id;  // Start then: a timer counting into a variable
    const u32 add = g.add("Add", 600, 100)->id;
    const u32 get = g.add("GetVariable", 400, 100)->id;
    const u32 store = g.add("SetVariable", 800, 0)->id;
    std::snprintf(g.find(every)->values[1], 64, "0.5");
    std::snprintf(g.find(add)->values[1], 64, "1");
    std::snprintf(g.find(get)->values[0], 64, "ticks");
    std::snprintf(g.find(store)->values[1], 64, "ticks");
    CHECK(g.connect(set, 0, every, 0) && g.connect(every, 1, store, 0) && g.connect(get, 0, add, 0) && g.connect(add, 0, store, 2));

    // Save, load, save again: same text.
    Array<char> text(default_allocator()), again(default_allocator());
    save_graph(g, text);
    Graph loaded(default_allocator());
    CHECK(load_graph(loaded, text.data(), text.size()));
    save_graph(loaded, again);
    CHECK(text.size() == again.size() && std::memcmp(text.data(), again.data(), text.size()) == 0);
    CHECK(loaded.nodes.size() == g.nodes.size() && loaded.links.size() == g.links.size() && loaded.next_id == g.next_id);

    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(loaded, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());

    HeapAllocator heap("graph");
    {
        World w(heap);
        const Entity e = w.create("Drone");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "graph");
        std::snprintf(w.add<Tags>(e)->tags, sizeof(Tags::tags), "Enemy");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        CHECK(scripts.error_count() == 0);
        const Vec3 p = w.get<Transform>(e)->position;
        CHECK(p.x == 1.0f && p.y == 2.5f && p.z == 0.0f);
        for (int i = 0; i < 10; ++i) scripts.update(0.1f);
        CHECK(std::fabs(w.get<Transform>(e)->rotation.z - 90.0f) < 0.01f);  // 1 s at 90 deg/s
        CHECK(scripts.run("assert(true)"));
        w.get<Tags>(e)->tags[0] = '\0';  // not an Enemy: prints instead of spinning
        scripts.update(0.1f);
        CHECK(std::fabs(w.get<Transform>(e)->rotation.z - 90.0f) < 0.01f && scripts.error_count() == 0);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);

    // Problems are reported with the node.
    const u32 a = g.add("Sequence", 0, 700)->id, b = g.add("Sequence", 200, 700)->id;
    CHECK(g.connect(a, 0, b, 0) && g.connect(b, 0, a, 0) && g.connect(start, 0, a, 0));
    CHECK(!compile_graph(g, lua, error, sizeof error) && std::strstr(error, "loop"));
    g.remove(a);
    CHECK(g.link_into(b, 0) == nullptr);
    g.add("OnStart", 0, 900);
    CHECK(!compile_graph(g, lua, error, sizeof error) && std::strstr(error, "twice"));
}

int main() {
    test_graph();
    write(kMover, R"(
function on_start(self)
  self.target = lsd.find("Target")
  self.ticks = 0
  local x, y, z = lsd.position(self.entity)
  lsd.set_position(self.entity, x, y, 10)
  print("started", self.name, lsd.has_tag(self.target, "Enemy"))
end
function on_update(self, dt)
  self.ticks = self.ticks + 1
  local x, y, z = lsd.position(self.entity)
  local mx, my = lsd.input("IA_Move")
  lsd.set_position(self.entity, x + dt * my, y, z)
  if lsd.pressed("IA_Jump") and lsd.has_tag(self.target, "Enemy") then lsd.set_rotation(self.target, 0, 0, 90) end
end
)");
    write(kBroken, "function on_update(self, dt) error('boom') end\n");

    HeapAllocator heap("script");
    {
        World w(heap);
        const Entity mover = w.create("Mover");
        std::snprintf(w.add<Script>(mover)->script, sizeof(Script::script), "mover");
        const Entity target = w.create("Target");
        std::snprintf(w.add<Tags>(target)->tags, sizeof(Tags::tags), "Enemy.Orc");
        const Entity broken = w.create("Broken");
        std::snprintf(w.add<Script>(broken)->script, sizeof(Script::script), "broken");

        script::ScriptSystem scripts;
        script::ScriptHost host;
        host.input = input;
        host.pressed = pressed;
        host.spawn = spawn;
        host.raycast = raycast;
        g_world = &w;
        scripts.start(w, host, path_of, nullptr);
        CHECK(scripts.script_count() == 2 && scripts.error_count() == 0);
        CHECK(w.get<Transform>(mover)->position.z == 10.0f);  // on_start ran

        for (int i = 0; i < 10; ++i) scripts.update(0.1f);
        CHECK(std::fabs(w.get<Transform>(mover)->position.x - 1.0f) < 1e-4f);  // 10 x 0.1 s x move.y 1
        CHECK(w.get<Transform>(target)->rotation.z == 90.0f);
        CHECK(scripts.error_count() == 1);  // the broken one stopped, the mover kept going

        // Reload: a new on_update keeps self's fields (ticks counted so far).
        write(kMover, "function on_update(self, dt) lsd.set_position(self.entity, self.ticks, 0, 0) end\n");
        std::filesystem::last_write_time(kMover, std::filesystem::last_write_time(kMover) + std::chrono::seconds(5));
        scripts.update(0.5f);
        CHECK(w.get<Transform>(mover)->position.x == 10.0f);

        CHECK(scripts.run("lsd.set_position(lsd.find('Target'), 1, 2, 3)"));
        CHECK(w.get<Transform>(target)->position.y == 2.0f);
        CHECK(!scripts.run("lsd.position(12345)"));  // bad entity: an error, not a crash
        CHECK(!scripts.run("this is not lua"));

        // Any component through reflection.
        CHECK(scripts.run(R"(
local t = lsd.find("Target")
assert(lsd.get(t, "RigidBody") == nil and not lsd.has(t, "RigidBody"))
lsd.add(t, "RigidBody", {mass = 3})
assert(lsd.has(t, "RigidBody") and lsd.get(t, "RigidBody").mass == 3 and lsd.get(t, "RigidBody").friction == 0.5)
lsd.set(t, "Transform", {scale = {z = 4}})
local tr = lsd.get(t, "Transform")
assert(tr.scale.x == 1 and tr.scale.z == 4 and tr.position.y == 2)
lsd.set(t, "Tags", {tags = "Friend"})
assert(lsd.has_tag(t, "Friend"))
)"));
        CHECK(w.get<RigidBody>(target) && w.get<RigidBody>(target)->mass == 3.0f && w.get<Transform>(target)->scale.z == 4.0f);
        CHECK(!scripts.run("lsd.set(lsd.find('Target'), 'RigidBody', {mass = 'heavy', friction = 0.9})"));
        CHECK(w.get<RigidBody>(target)->friction == 0.5f);  // a bad field changes nothing
        CHECK(!scripts.run("lsd.set(lsd.find('Target'), 'RigidBody', {weight = 1})"));
        CHECK(!scripts.run("lsd.get(lsd.find('Target'), 'Banana')"));
        CHECK(scripts.run("lsd.remove(lsd.find('Target'), 'RigidBody')") && !w.get<RigidBody>(target));

        // Timers: after fires once, every repeats until cancelled.
        CHECK(scripts.run(R"(
once, many = 0, 0
lsd.after(0.25, function() once = once + 1 end)
local id
id = lsd.every(0.1, function() many = many + 1; if many == 3 then lsd.cancel(id) end end)
)"));
        for (int i = 0; i < 10; ++i) scripts.update(0.1f);
        CHECK(scripts.run("assert(once == 1, once) assert(many == 3, many)"));

        // Events go to the target's scripts with the other object.
        write(kMover, "function on_overlap_begin(self, other) lsd.set_position(self.entity, 0, 0, 0); _G.hit = lsd.name(other) end\n");
        std::filesystem::last_write_time(kMover, std::filesystem::last_write_time(kMover) + std::chrono::seconds(10));
        scripts.update(0.5f);
        scripts.send(mover, "on_overlap_begin", target);
        scripts.send(mover, "on_not_defined", target);  // fine: scripts need not handle every event
        CHECK(scripts.run("assert(hit == 'Target', hit)") && w.get<Transform>(mover)->position.x == 0.0f);

        // Spawning: the new object's script starts (its on_overlap_begin is defined, on_start is not: fine).
        write(kMover, "function on_start(self) _G.started = self.name end\n");
        std::filesystem::last_write_time(kMover, std::filesystem::last_write_time(kMover) + std::chrono::seconds(15));
        scripts.update(0.5f);
        CHECK(scripts.run(R"(
local e = lsd.spawn("Mover", 1, 2, 3)
assert(e and lsd.name(e) == "Spawned" and select(3, lsd.position(e)) == 3)
assert(lsd.spawn("Nothing", 0, 0, 0) == nil)
)"));
        const u32 before = scripts.script_count();
        scripts.attach(w.entity_at_slot(w.slot_count() - 1));
        CHECK(scripts.script_count() == before + 1 && scripts.run("assert(started == 'Spawned', started)"));
        CHECK(scripts.run(R"(
local e, x, y, z, nx, ny, nz = lsd.raycast(0, 0, 5, 0, 0, -1, 10)
assert(e == lsd.find("Mover") and z == 5 and nz == 1)
assert(lsd.raycast(0, 0, -5, 0, 0, -1, 10) == nil)
)"));
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
    if (g_failures == 0) std::printf("All script tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
