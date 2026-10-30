// Script tests: a Lua file moves its object, finds others, reads tags and input, survives errors
// and reloads when the file changes.
#include "lsd/asset/data_table.h"
#include "lsd/script/graph.h"
#include "lsd/script/material_graph.h"
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
static const char kTable[] = LSD_TEST_TEMP_DIR "/DT_Weapons.csv";
static const char kCurve[] = LSD_TEST_TEMP_DIR "/C_Ramp.lsdasset";

static bool curve_path(void*, const char* name, char* out, usize capacity) {
    if (std::strcmp(name, "C_Ramp") != 0) return false;
    std::snprintf(out, capacity, "%s", kCurve);
    return true;
}

static bool table_path(void*, const char* name, char* out, usize capacity) {
    if (std::strcmp(name, "DT_Weapons") != 0) return false;
    std::snprintf(out, capacity, "%s", kTable);
    return true;
}

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

static void write(const char* path, const char* text);

// Material graphs compile to one shader function; problems name the node.
static void test_material_graph() {
    using namespace lsd::script;
    Graph g(default_allocator());
    MaterialCode code(default_allocator());
    char error[256] = "";
    CHECK(!compile_material_graph(g, code, error, sizeof error) && std::strstr(error, "Output"));
    const u32 out = g.add("MatOutput", 400, 0)->id;
    CHECK(compile_material_graph(g, code, error, sizeof error));
    CHECK(std::strstr(code.code.data(), "s.base_color = float3(0.8, 0.8, 0.8);"));
    CHECK(std::strstr(code.code.data(), "s.metallic = (lsd_v3(0.0)).x;"));  // "0" typed: still a float literal

    // Texture x Color into Base Color; the same texture as a Normal map is a second slot.
    GraphNode* tex = g.add("MatTexture", 0, 0);
    std::snprintf(tex->values[1], sizeof tex->values[1], "T_Rock");
    const u32 tex_id = tex->id;
    const u32 tint = g.add("MatColor", 0, 100)->id;
    const u32 mul = g.add("MatMultiply", 200, 0)->id;
    CHECK(g.connect(tex_id, 0, mul, 0));
    CHECK(g.connect(tint, 0, mul, 1));
    CHECK(g.connect(mul, 0, out, 0));
    CHECK(g.connect(tex_id, 1, out, 2));  // a Float output into a Float input
    const u32 time = g.add("MatTime", 0, 200)->id;
    CHECK(g.connect(time, 0, mul, 0));    // a Float output into a Vector input (material nodes mix)
    CHECK(g.connect(tex_id, 0, mul, 0));
    GraphNode* normal = g.add("MatTexture", 0, 300);
    std::snprintf(normal->values[1], sizeof normal->values[1], "T_Rock");
    std::snprintf(normal->values[2], sizeof normal->values[2], "Normal");
    CHECK(g.connect(normal->id, 0, out, 4));
    CHECK(compile_material_graph(g, code, error, sizeof error));
    CHECK(code.texture_count == 2 && code.textures[0].srgb && !code.textures[1].srgb);
    CHECK(std::strstr(code.code.data(), "lsd_tex0(mi.uv).rgb"));
    CHECK(std::strstr(code.code.data(), "(lsd_tex1(mi.uv).rgb * 2.0 - lsd_v3(1.0))"));
    CHECK(std::strstr(code.code.data(), "s.roughness = (v"));

    // Parameters: one slot per name, defaults kept for the instances.
    GraphNode* bright = g.add("MatScalarParam", 0, 700);
    std::snprintf(bright->values[1], sizeof bright->values[1], "2.5");
    const u32 bright_id = bright->id;
    GraphNode* tint_param = g.add("MatVectorParam", 0, 800);
    std::snprintf(tint_param->values[1], sizeof tint_param->values[1], "0.1,0.2,0.3");
    const u32 tint_param_id = tint_param->id;
    CHECK(g.connect(bright_id, 0, out, 1) && g.connect(tint_param_id, 0, mul, 1));
    CHECK(compile_material_graph(g, code, error, sizeof error));
    CHECK(code.param_count == 2 && std::strcmp(code.params[0].name, "Tint") == 0 && code.params[0].vector && code.params[0].value[2] == 0.3f);
    CHECK(std::strcmp(code.params[1].name, "Brightness") == 0 && code.params[1].value[0] == 2.5f && code.params[1].value[2] == 2.5f);
    CHECK(std::strstr(code.code.data(), "lsd_params[0].xyz") && std::strstr(code.code.data(), "lsd_v3(lsd_params[1].x)"));

    // A loop and a texture without a name are reported.
    const u32 a = g.add("MatAdd", 0, 400)->id, b = g.add("MatAdd", 0, 500)->id;
    CHECK(g.connect(a, 0, b, 0) && g.connect(b, 0, a, 0) && g.connect(b, 0, out, 3));
    CHECK(!compile_material_graph(g, code, error, sizeof error) && std::strstr(error, "loop"));
    g.disconnect_input(out, 3);
    std::snprintf(normal->values[1], sizeof normal->values[1], "%s", "");
    CHECK(!compile_material_graph(g, code, error, sizeof error) && std::strstr(error, "no texture"));
    // Scripts still keep their own types: a float does not wire into a vector there.
    Graph s(default_allocator());
    const u32 dt = s.add("OnUpdate", 0, 0)->id, pos = s.add("SetPosition", 200, 0)->id;
    CHECK(!s.connect(dt, 1, pos, 1));
}

static void test_data_table() {
    asset::DataTable t(default_allocator());
    const char csv[] = "\xEF\xBB\xBFName,Damage,Label\r\nPistol,12.5,\"Small, quiet\"\r\nRifle,40,\"Says \"\"bang\"\"\nloudly\"\n\nShort\n";
    CHECK(t.parse(csv, sizeof csv - 1));
    CHECK(t.rows() == 4 && t.columns() == 3);
    CHECK(std::strcmp(t.cell(0, 0), "Name") == 0 && std::strcmp(t.cell(1, 2), "Small, quiet") == 0);
    CHECK(std::strcmp(t.cell(2, 2), "Says \"bang\"\nloudly") == 0);
    CHECK(std::strcmp(t.cell(3, 0), "Short") == 0 && std::strcmp(t.cell(3, 1), "") == 0);  // padded
    CHECK(t.find_row("Rifle") == 2 && t.find_row("Name") == -1 && t.find_column("Damage") == 1);
    const char trailing[] = "a,b,\n1,2,";
    CHECK(t.parse(trailing, sizeof trailing - 1) && t.columns() == 3 && t.rows() == 2);
    CHECK(!t.parse("a,\"open", 8));

    // Write and read back.
    const char* cells[] = {"Name", "Note", "x", "a, \"b\"\nc"};
    Array<char> out(default_allocator());
    asset::write_csv(cells, 2, 2, out);
    CHECK(t.parse(out.data(), out.size()) && std::strcmp(t.cell(1, 1), cells[3]) == 0);

    // From Lua, cached until the file changes.
    write(kTable, "Name,Damage,Label\nPistol,12.5,Small\nRifle,40,Big\n");
    HeapAllocator heap("table");
    {
        World w(heap);
        script::ScriptSystem scripts;
        script::ScriptHost host;
        host.table_path = table_path;
        host.curve_path = curve_path;
        scripts.start(w, host, path_of, nullptr);
        // Curves: linear 0..10 over 2 s, read through the data asset file.
        write(kCurve, R"({"type": "Curve", "data": {"keys": [{"time": 2, "value": 10, "interp": 1}, {"time": 0, "value": 0, "interp": 1}]}})");
        CHECK(scripts.run("assert(math.abs(lsd.curve('C_Ramp', 0.5) - 2.5) < 1e-4 and lsd.curve('C_Ramp', 9) == 10 and lsd.curve('Nope', 1) == 0)"));
        CHECK(scripts.run(R"(
local t = lsd.table("DT_Weapons")
assert(t.Pistol.Damage == 12.5 and t.Rifle.Label == "Big" and lsd.table("Nope") == nil)
assert(lsd.table("DT_Weapons") == t)  -- same table while the file is unchanged
)"));
        write(kTable, "Name,Damage\nPistol,99\n");
        std::filesystem::last_write_time(kTable, std::filesystem::last_write_time(kTable) + std::chrono::seconds(5));
        CHECK(scripts.run("assert(lsd.table('DT_Weapons').Pistol.Damage == 99)"));
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

int main() {
    test_data_table();
    test_graph();
    test_material_graph();
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
        // Splines in world space (the object's Transform applies).
        w.add<Spline>(target);  // a 4 m line along X
        CHECK(scripts.run(R"(
local t = lsd.find("Target")
lsd.set_rotation(t, 0, 0, 0)  -- an earlier test turned it
assert(math.abs(lsd.spline_length(t) - 4) < 0.01, lsd.spline_length(t))
local x, y, z, dx, dy, dz = lsd.spline_point(t, 2)
local px, py, pz = lsd.position(t)
assert(math.abs(x - (px + 2)) < 0.05 and math.abs(y - py) < 0.01 and dx > 0.99, x)
)"));
        CHECK(!scripts.run("lsd.spline_point(lsd.find('Mover'), 1)"));  // no Spline: an error

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
