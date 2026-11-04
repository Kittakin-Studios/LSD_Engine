// Script tests: a Lua file moves its object, finds others, reads tags and input, survives errors
// and reloads when the file changes.
#include "lsd/asset/data_table.h"
#include "lsd/script/graph.h"
#include "lsd/script/material_graph.h"
#include "lsd/script/script.h"
#include "lsd/world/level.h"
#include "lsd/world/user_types.h"
#include "lsd/asset/data_asset.h"
#include "lsd/game.hpp"

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
static const char kPropsLua[] = LSD_TEST_TEMP_DIR "/props.lua";
static const char kDebugLua[] = LSD_TEST_TEMP_DIR "/debugged.lua";
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
    if (std::strncmp(guid, "file:", 5) == 0) {  // any file in the temp folder
        std::snprintf(out, capacity, "%s/%s", LSD_TEST_TEMP_DIR, guid + 5);
        return true;
    }
    std::snprintf(out, capacity, "%s", std::strcmp(guid, "mover") == 0 ? kMover : std::strcmp(guid, "graph") == 0 ? kGraphLua : std::strcmp(guid, "props") == 0 ? kPropsLua : std::strcmp(guid, "debugged") == 0 ? kDebugLua : kBroken);
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
// Declared variables (My Blueprint): saved with the graph; compiled to `properties` (Instance
// Editable numbers / booleans / text) and `defaults` (the rest), which the script starts with.
static void test_graph_variables() {
    using namespace lsd::script;
    Graph g(default_allocator());
    g.add("OnStart", 0, 0);
    GraphVariable speed;
    std::snprintf(speed.name, sizeof speed.name, "speed");
    std::snprintf(speed.default_value, sizeof speed.default_value, "5");
    speed.instance_editable = true;
    GraphVariable home;
    std::snprintf(home.name, sizeof home.name, "home");
    home.type = PinType::Vector;
    std::snprintf(home.default_value, sizeof home.default_value, "1,2,3");
    home.instance_editable = true;  // vectors cannot be edited per object: a default
    GraphVariable label;
    std::snprintf(label.name, sizeof label.name, "label");
    label.type = PinType::String;
    std::snprintf(label.default_value, sizeof label.default_value, "Say \"hi\"");
    g.variables.push_back(speed);
    g.variables.push_back(home);
    g.variables.push_back(label);
    Array<char> text(default_allocator());
    save_graph(g, text);
    Graph back(default_allocator());
    CHECK(load_graph(back, text.data(), text.size()) && back.variables.size() == 3);
    if (back.variables.size() == 3) {
        CHECK(std::strcmp(back.variables[1].name, "home") == 0 && back.variables[1].type == PinType::Vector && back.variables[0].instance_editable);
        CHECK(std::strcmp(back.variables[2].default_value, "Say \"hi\"") == 0);
    }
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(back, lua, error, sizeof error));
    lua.push_back('\0');
    CHECK(std::strstr(lua.data(), "properties = { speed = 5, }") != nullptr);
    CHECK(std::strstr(lua.data(), "defaults = { home = __vec(1, 2, 3), label = \"Say \\\"hi\\\"\", }") != nullptr);
    // The script starts with them on self.
    World w(default_allocator());
    script::ScriptSystem scripts;
    scripts.start(w, {}, nullptr, nullptr);
    std::string chunk(lua.data());
    chunk += "\nassert(properties.speed == 5 and defaults.home.y == 2 and defaults.label == 'Say \"hi\"')";
    CHECK(scripts.run(chunk.c_str()));
    scripts.stop();
}

// Collapse to Function: the same result after the nodes move into a Function, wires rerouted.
static void test_collapse_to_function() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 s1 = g.add("SetVariable", 200, 0)->id, s2 = g.add("SetVariable", 400, 0)->id, pos = g.add("SetPosition", 600, 0)->id;
    const u32 get_a = g.add("GetVariable", 200, 150)->id, mul = g.add("Multiply", 350, 150)->id, add = g.add("Add", 500, 150)->id;
    const u32 get_b = g.add("GetVariable", 500, 250)->id, vec = g.add("MakeVector", 600, 150)->id;
    std::snprintf(g.find(s1)->values[1], 64, "a");
    std::snprintf(g.find(s1)->values[2], 64, "5");
    std::snprintf(g.find(s2)->values[1], 64, "b");
    std::snprintf(g.find(get_a)->values[0], 64, "a");
    std::snprintf(g.find(get_b)->values[0], 64, "b");
    std::snprintf(g.find(mul)->values[1], 64, "3");
    std::snprintf(g.find(add)->values[1], 64, "1");
    CHECK(g.connect(start, 0, s1, 0) && g.connect(s1, 0, s2, 0) && g.connect(s2, 0, pos, 0) && g.connect(get_a, 0, mul, 0) &&
          g.connect(mul, 0, s2, 2) && g.connect(mul, 0, add, 0) && g.connect(add, 0, vec, 0) && g.connect(get_b, 0, vec, 1) &&
          g.connect(vec, 0, pos, 2));
    // Not collapsible: an event inside, or no white wire in. The graph stays as it was.
    char error[160];
    u32 call = 0;
    const usize nodes = g.nodes.size(), links = g.links.size();
    const u32 with_event[] = {start, s1};
    CHECK(!collapse_to_function(g, with_event, 2, "F", call, error, sizeof error) && std::strstr(error, "event"));
    CHECK(!collapse_to_function(g, &mul, 1, "F", call, error, sizeof error) && std::strstr(error, "white"));
    CHECK(g.nodes.size() == nodes && g.links.size() == links);

    const u32 body[] = {s2, mul};
    CHECK(collapse_to_function(g, body, 2, "Triple", call, error, sizeof error));
    CHECK(g.nodes.size() == nodes + 3 && g.find(call) && std::strcmp(g.find(call)->values[1], "Triple") == 0);
    Array<char> lua(default_allocator());
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    CHECK(std::strstr(lua.data(), "function Triple(self, a, b, c)"));
    write(kGraphLua, lua.data());
    HeapAllocator heap("collapse");
    {
        World w(heap);
        const Entity e = w.create("Thing");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "graph");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        const Vec3 p = w.get<Transform>(e)->position;
        CHECK(scripts.error_count() == 0 && p.x == 16.0f && p.y == 15.0f);  // b = a * 3 inside; a * 3 + 1 read through Result
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

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
    // Every problem at once, a line each (Compiler Results).
    g.add("OnStart", 0, 1100);
    CHECK(!compile_graph(g, lua, error, sizeof error));
    int lines = 0;
    for (const char* c = error; *c; ++c) lines += *c == '\n';
    CHECK(lines == 2 && std::strstr(error, "(node "));
}

static void write(const char* path, const char* text);

static u32 g_sweeps = 0;
static bool sweep(void*, int, Vec3, Vec3 from, Vec3, Entity& hit, Vec3& point, Vec3& normal) {
    ++g_sweeps;
    hit = g_world->entity_at_slot(0);
    point = from;
    normal = {0, 0, 1};
    return true;
}
static u32 overlap(void*, int, Vec3, Vec3, Entity* out, u32 capacity) {  // every object in the world
    u32 n = 0;
    for (u32 slot = 0; slot < g_world->slot_count() && n < capacity; ++slot) {
        if (g_world->entity_at_slot(slot).valid()) out[n++] = g_world->entity_at_slot(slot);
    }
    return n;
}

// Collision nodes: Sphere Trace, then Sphere Overlap's Loop Body moves every object it found up 1 m.
static void test_traces() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 trace = g.add("SphereTrace", 200, 0)->id;
    const u32 around = g.add("SphereOverlap", 400, 0)->id;
    const u32 lift = g.add("AddOffset", 600, 0)->id;
    CHECK(g.connect(start, 0, trace, 0) && g.connect(trace, 0, around, 0) && g.connect(around, 1, lift, 0) && g.connect(around, 2, lift, 1));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("traces");
    {
        World w(heap);
        const Entity a = w.create("A"), b = w.create("B");
        std::snprintf(w.add<Script>(a)->script, sizeof(Script::script), "graph");
        script::ScriptHost host;
        host.sweep = sweep;
        host.overlap = overlap;
        g_world = &w;
        script::ScriptSystem scripts;
        scripts.start(w, host, path_of, nullptr);
        CHECK(scripts.error_count() == 0 && g_sweeps == 1);
        CHECK(w.get<Transform>(a)->position.z == 1.0f && w.get<Transform>(b)->position.z == 1.0f);
        CHECK(scripts.run("local hits = lsd.overlap_box(0, 0, 0, 1, 1, 1) assert(#hits == 2) assert(lsd.capsule_trace(0, 0, 0, 1, 0, 0, 0.3, 0.5))"));
        scripts.stop();
    }
}

// ---- C / C++ behaviours (game module) ----

// A C++ behaviour as game code writes it: climbs at `speed` m/s, counts hits, answers "ping" with "pong".
struct Climber {
    float speed = 2.0f;
    int hits = 0;
    static void properties(lsd::game::Properties<Climber>& p) {
        p.add("speed", &Climber::speed);
        p.add("hits", &Climber::hits);
    }
    void on_update(lsd::game::Entity self, float dt) { lsd::game::set_position(self, lsd::game::position(self) + lsd::game::Vec3(0, 0, speed * dt)); }
    void on_hit(lsd::game::Entity self, lsd::game::Entity) { lsd::game::set_rotation(self, {0, 0, 10.0f * static_cast<float>(++hits)}); }
    void on_event(lsd::game::Entity self, const char* name, lsd::game::Entity other) {
        if (std::strcmp(name, "ping") == 0) lsd::game::call(other, "pong", self);
    }
};
LSD_SCRIPT(Climber)

// C++ functions as visual-script nodes.
static float lift_by(lsd::game::Entity target, float height) {
    lsd::game::Vec3 p = lsd::game::position(target);
    p.z += height;
    lsd::game::set_position(target, p);
    return p.z;
}
LSD_NODE(lift_by, "Moves", "target, height = 2", "Raises the object")
static float half(float value) { return value * 0.5f; }
LSD_PURE_NODE(half, "Math", "value", "")

// A component of the game's own.
struct Health {
    float hp = 100.0f;
    int32_t armor = 3;
    bool alive = true;
    char title[16] = "Knight";
    static void properties(lsd::game::Properties<Health>& p) {
        p.add("hp", &Health::hp);
        p.add("armor", &Health::armor);
        p.add("alive", &Health::alive);
        p.add("title", &Health::title);
    }
};
LSD_COMPONENT(Health, "Combat")
LSD_GAME_MODULE()

static void test_native_components() {
    HeapAllocator heap("components");
    {
        World early(heap);  // made before the type exists: its pool appears when first used
        const Entity knight = early.create("Knight");
        auto* module = new script::NativeModule();  // big: not on the stack
        CHECK(lsd_game_register(script::native_api(), module->registry()) == 1);
        CHECK(module->component_count == 1);
        CHECK(script::register_native_components(*module) == 1);
        CHECK(script::register_native_components(*module) == 0);  // already known: kept
        const reflect::TypeInfo* type = find_component_type("Health");
        CHECK(type && type->property_count == 4 && std::strcmp(type->category, "Combat") == 0);
        auto* health = type ? static_cast<Health*>(early.add(knight, *type)) : nullptr;
        CHECK(health && health->hp == 100.0f && health->armor == 3 && health->alive && std::strcmp(health->title, "Knight") == 0);

        // Lua reads and writes it by name, like a built-in component.
        script::ScriptSystem scripts;
        scripts.start(early, {}, path_of, nullptr);
        char line[200];
        std::snprintf(line, sizeof line, "local h = lsd.get(%u, 'Health') assert(h.hp == 100 and h.armor == 3 and h.title == 'Knight') lsd.set(%u, 'Health', {hp = 42, title = 'Paladin'})",
                      knight.value, knight.value);
        CHECK(scripts.run(line));
        scripts.stop();
        CHECK(health && health->hp == 42.0f && std::strcmp(health->title, "Paladin") == 0);

        // Visual scripts: the typed Get / Set nodes of its fields, and the generic Set Component Value.
        CHECK(script::register_native_nodes(*module) >= 8);
        CHECK(script::find_node_def("Cpp_Get_Health_hp") && script::find_node_def("Cpp_Set_Health_armor"));
        script::Graph g(default_allocator());
        const u32 start = g.add("OnStart", 0, 0)->id;
        const u32 set_armor = g.add("Cpp_Set_Health_armor", 200, 0)->id;
        const u32 read_hp = g.add("Cpp_Get_Health_hp", 0, 200)->id;
        const u32 set_title = g.add("SetComponentValue", 400, 0)->id;
        std::snprintf(g.find(set_title)->values[3], 64, "title");
        std::snprintf(g.find(set_title)->values[4], 64, "Hero");
        CHECK(g.connect(start, 0, set_armor, 0) && g.connect(read_hp, 0, set_armor, 2) && g.connect(set_armor, 0, set_title, 0));
        Array<char> lua(default_allocator());
        char error[256];
        CHECK(script::compile_graph(g, lua, error, sizeof error));
        lua.push_back('\0');
        write(kGraphLua, lua.data());
        std::snprintf(early.add<Script>(knight)->script, sizeof(Script::script), "graph");
        script::ScriptSystem graph_scripts;
        graph_scripts.start(early, {}, path_of, nullptr);
        graph_scripts.stop();
        CHECK(health && health->armor == 42 && std::strcmp(health->title, "Hero") == 0);  // armor = hp; title by name
        script::clear_custom_nodes();

        // Saved with the level and read back.
        Array<char> text(heap);
        save_level(early, text);
        World later(heap);
        CHECK(load_level(later, text.data(), text.size()));
        const Entity again = later.entity_at_slot(knight.index());
        const auto* loaded = type && again.valid() ? static_cast<const Health*>(later.get(again, *type)) : nullptr;
        CHECK(loaded && loaded->hp == 42.0f && loaded->armor == 42 && std::strcmp(loaded->title, "Hero") == 0);
        delete module;
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_native_nodes() {
    using namespace lsd::script;
    NativeModule module;
    CHECK(lsd_game_register(native_api(), module.registry()) == 1);
    CHECK(module.find_function("lift_by") && module.find_function("half") && module.find_function("half")->pure);
    CHECK(register_native_nodes(module) == 2 + 8);  // two functions, a Get and a Set per Health field
    const NodeDef* lift = find_node_def("Cpp_lift_by");
    CHECK(lift && lift->input_count == 3 && lift->output_count == 2 && std::strcmp(lift->inputs[1].default_value, "self") == 0 &&
          std::strcmp(lift->inputs[2].default_value, "2") == 0 && std::strcmp(lift->category, "Moves") == 0);
    // Start -> lift by half(8) = 4 -> lift by the default 2: z = 6.
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 first = g.add("Cpp_lift_by", 200, 0)->id;
    const u32 halved = g.add("Cpp_half", 0, 200)->id;
    const u32 second = g.add("Cpp_lift_by", 400, 0)->id;
    std::snprintf(g.find(halved)->values[0], 64, "8");
    std::snprintf(g.find(second)->values[2], 64, "2");
    CHECK(g.connect(start, 0, first, 0) && g.connect(halved, 0, first, 2) && g.connect(first, 0, second, 0));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("native nodes");
    {
        World w(heap);
        const Entity e = w.create("E");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "graph");
        ScriptSystem scripts;
        scripts.set_native_module(&module);
        scripts.start(w, {}, path_of, nullptr);
        CHECK(scripts.error_count() == 0 && w.get<Transform>(e)->position.z == 6.0f);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
    clear_custom_nodes();
}

// A C behaviour registered by hand, as a .c file would.
static void c_start(lsd_entity self, void* state) {
    *static_cast<int*>(state) = 5;
    lsd_set_number(self, "Transform", "nonexistent", 1.0);  // unknown fields are ignored
    lsd_vec3 p = lsd_position(self);
    p.x = static_cast<float>(*static_cast<int*>(state));
    lsd_set_position(self, p);
}

// The module registers both; Native Script components run them in Play with the Lua scripts, and
// events cross between C++, C and Lua in both directions.
static void test_native_behaviours() {
    script::NativeModule module;
    CHECK(lsd_game_register(script::native_api(), module.registry()) == 1);
    lsd_behaviour c{};
    c.name = "Pusher";
    c.state_size = sizeof(int);
    c.on_start = c_start;
    lsd_api_ptr->register_behaviour(module.registry(), &c);
    CHECK(module.count == 3 && module.find("Climber") && module.find("CMover") && module.find("Pusher") && module.find("Climber")->property_count == 2);
    char text[32];
    script::native_property_default(*module.find("Climber"), module.find("Climber")->properties[0], text, sizeof text);
    CHECK(std::strcmp(text, "2") == 0);

    // B's Lua script pings A at start; A's C++ answers with pong, which moves B.
    write(kGraphLua, "function on_start(self) lsd.call(lsd.find('A'), 'ping', self.entity) end\n"
                     "function pong(self, from) lsd.set_position(self.entity, 0, 0, 7) end\n");
    HeapAllocator heap("native");
    {
        World w(heap);
        const Entity a = w.create("A"), b = w.create("B"), p = w.create("P");
        auto* native = w.add<NativeScript>(a);
        std::snprintf(native->script, sizeof native->script, "Climber");
        ScriptProperty speed;
        std::snprintf(speed.name, sizeof speed.name, "speed");
        std::snprintf(speed.value, sizeof speed.value, "4");
        native->properties.push_back(speed);
        std::snprintf(w.add<Script>(b)->script, sizeof(Script::script), "graph");
        std::snprintf(w.add<NativeScript>(p)->script, sizeof(NativeScript::script), "Pusher");
        const Entity m = w.create("M");
        std::snprintf(w.add<NativeScript>(m)->script, sizeof(NativeScript::script), "CMover");
        script::ScriptHost host;
        script::ScriptSystem scripts;
        scripts.set_native_module(&module);
        scripts.start(w, host, path_of, nullptr);
        CHECK(scripts.native_count() == 3 && scripts.error_count() == 0);
        // Any lsd.* command from C / C++: by name, arguments and results as JSON.
        const lsd::game::ApiResult where = lsd::game::api("position", a.value);
        CHECK(where.ok && where.number(2) == w.get<Transform>(a)->position.z);
        CHECK(lsd::game::api("name", a.value).text() == "A");
        CHECK(lsd::game::api("has_tag", a.value, "Nope").ok && !lsd::game::api("has_tag", a.value, "Nope").boolean());
        CHECK(!lsd::game::api("no_such_command").ok && !lsd::game::api("_tick").ok);  // unknown and hidden: refused
        char out[128];
        CHECK(lsd_lua_call("set_position", "[1, 2, 3]", out, sizeof out) == 0);  // 1 is not a live object: an error, not a crash
        std::snprintf(out, sizeof out, "[%u, 0, 0, 9]", a.value);
        char result[64];
        CHECK(lsd_lua_call("set_position", out, result, sizeof result) == 1 && w.get<Transform>(a)->position.z == 9.0f && std::strcmp(result, "[]") == 0);
        w.get<Transform>(a)->position = {};  // back where it was: the checks below measure from there
        CHECK(w.get<Transform>(b)->position.z == 7.0f);   // Lua -> C++ -> Lua
        CHECK(w.get<Transform>(p)->position.x == 5.0f);   // the C behaviour started
        scripts.update(0.5f);
        CHECK(std::fabs(w.get<Transform>(a)->position.z - 2.0f) < 1e-5f);  // speed 4 from the object's override
        CHECK(std::fabs(w.get<Transform>(m)->position.x - 0.5f) < 1e-5f);  // the self-registered C script ran
        CHECK(scripts.send(a, "on_hit", b) == 1 && scripts.send(a, "on_hit", b) == 1);
        CHECK(w.get<Transform>(a)->rotation.z == 20.0f);
        CHECK(scripts.run("assert(lsd.call(lsd.find('A'), 'ping', lsd.find('B')) == 1)"));  // reaches the behaviour
        scripts.stop_natives();  // a module reload: stop, swap, start again
        CHECK(scripts.native_count() == 0);
        scripts.start_natives();
        CHECK(scripts.native_count() == 3);
        CHECK(scripts.send(a, "on_hit", b) == 1 && w.get<Transform>(a)->rotation.z == 30.0f);  // hits kept through the reload (2 + 1)
        scripts.stop();
        CHECK(scripts.native_count() == 0);
    }
}

// Get Forward Vector: an object turned 90 degrees about Z faces +Y, so Start moves it to (0, 1, 0).
static void test_forward_vector() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 move = g.add("SetPosition", 200, 0)->id;
    const u32 forward = g.add("GetForward", 0, 100)->id;
    CHECK(g.connect(start, 0, move, 0) && g.connect(forward, 0, move, 2));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("forward");
    {
        World w(heap);
        const Entity a = w.create("A");
        w.get<Transform>(a)->rotation = {0, 0, 90};
        std::snprintf(w.add<Script>(a)->script, sizeof(Script::script), "graph");
        script::ScriptHost host;
        script::ScriptSystem scripts;
        scripts.start(w, host, path_of, nullptr);
        const Vec3 p = w.get<Transform>(a)->position;
        CHECK(scripts.error_count() == 0 && std::fabs(p.x) < 1e-4f && std::fabs(p.y - 1.0f) < 1e-4f && std::fabs(p.z) < 1e-4f);
        scripts.stop();
    }
}

// Functions, a For Loop, reroutes and a comment: Start sums add_one(i) for i = 0..2 into the "sum" variable (1 + 2 + 3).
static void test_functions() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 fn = g.add("FunctionEntry", 0, 300)->id;  // add_one(a) = a + 1
    std::snprintf(g.find(fn)->values[0], 64, "add_one");
    const u32 plus = g.add("Add", 200, 300)->id;
    std::snprintf(g.find(plus)->values[1], 64, "1");
    const u32 ret = g.add("Return", 400, 300)->id;
    const u32 reroute = g.add("Reroute", 100, 350)->id;
    CHECK(g.connect(fn, 0, ret, 0) && g.connect(fn, 1, reroute, 0) && g.connect(reroute, 0, plus, 0) && g.connect(plus, 0, ret, 1));

    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 loop = g.add("ForLoop", 200, 0)->id;
    std::snprintf(g.find(loop)->values[2], 64, "2");
    const u32 call = g.add("CallFunction", 400, 0)->id;
    std::snprintf(g.find(call)->values[1], 64, "add_one");
    const u32 get = g.add("GetVariable", 400, 150)->id;
    std::snprintf(g.find(get)->values[0], 64, "sum");
    const u32 sum = g.add("Add", 600, 150)->id;
    const u32 set = g.add("SetVariable", 800, 0)->id;
    std::snprintf(g.find(set)->values[1], 64, "sum");
    CHECK(g.connect(start, 0, loop, 0) && g.connect(loop, 0, call, 0) && g.connect(loop, 1, call, 2) && g.connect(call, 0, set, 0));
    CHECK(g.connect(get, 0, sum, 0) && g.connect(call, 1, sum, 1) && g.connect(sum, 0, set, 2));
    // Completed: x = sum, so the test can read it.
    const u32 place = g.add("SetPosition", 400, -150)->id;
    const u32 make = g.add("MakeVector", 200, -150)->id;
    const u32 total = g.add("GetVariable", 0, -150)->id;
    std::snprintf(g.find(total)->values[0], 64, "sum");
    CHECK(g.connect(loop, 2, place, 0) && g.connect(total, 0, make, 0) && g.connect(make, 0, place, 2));
    GraphNode* note = g.add("Comment", -50, -50);
    std::snprintf(note->values[0], 64, "Adds things up");
    std::snprintf(note->values[1], 64, "900");

    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    // The comment keeps its text and size through a save.
    Array<char> text(default_allocator());
    save_graph(g, text);
    Graph again(default_allocator());
    CHECK(load_graph(again, text.data(), text.size()));
    const GraphNode* kept = nullptr;
    for (const GraphNode& n : again.nodes) kept = std::strcmp(n.type, "Comment") == 0 ? &n : kept;
    CHECK(kept && std::strcmp(kept->values[0], "Adds things up") == 0 && std::strcmp(kept->values[1], "900") == 0);

    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("functions");
    World w(heap);
    const Entity a = w.create("A");
    std::snprintf(w.add<Script>(a)->script, sizeof(Script::script), "graph");
    script::ScriptSystem scripts;
    scripts.start(w, {}, path_of, nullptr);
    CHECK(scripts.error_count() == 0);
    CHECK(w.get<Transform>(a)->position.x == 6.0f);  // 1 + 2 + 3
    scripts.stop();
}

// Game Instance: kept tables survive stop / start (a level change) until a new Play clears them.
static void test_game_instance() {
    HeapAllocator heap("instance");
    World w(heap);
    script::ScriptSystem scripts;
    scripts.start(w, {}, path_of, nullptr);
    CHECK(scripts.run("lsd.game_instance().score = 5 lsd.player_state().name = 'Ada' lsd.subsystem('Quests').done = {1, 2}"));
    scripts.stop();
    scripts.start(w, {}, path_of, nullptr);  // the next level
    CHECK(scripts.run("assert(lsd.game_instance().score == 5 and lsd.player_state().name == 'Ada' and #lsd.subsystem('Quests').done == 2)"));
    CHECK(scripts.run("assert(lsd.player() == nil and lsd.open_level('Nowhere') == false)"));  // no host: no player, no levels
    // No string table: the key is the pattern; extra arguments fill it in, numbers pick plurals.
    CHECK(scripts.run("assert(lsd.text('{0} {0}|plural(one=coin,other=coins), {1}!', 3, true) == '3 coins, true!' and lsd.text('{0} left', 1) == '1 left')"));
    // The sandbox: no files, processes, native code or bytecode; the harmless parts remain.
    CHECK(scripts.run("assert(io == nil and os.execute == nil and os.remove == nil and dofile == nil and string.dump == nil)"));
    CHECK(scripts.run("assert(package.loadlib == nil and not pcall(require, 'ffi') and type(os.time()) == 'number')"));
    CHECK(scripts.run("assert(load('return 1 + 1')() == 2 and type(debug.traceback) == 'function' and debug.sethook == nil)"));
    scripts.stop();
    scripts.clear_persistent();  // a new Play
    scripts.start(w, {}, path_of, nullptr);
    CHECK(scripts.run("assert(lsd.game_instance().score == nil)"));
    scripts.stop();
}

// Event dispatchers: Call Event reaches every object's Custom Event; lsd.call reaches one.
static void test_events() {
    using namespace lsd::script;
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 send = g.add("CallEvent", 200, 0)->id;
    std::snprintf(g.find(send)->values[1], 64, "ping");
    std::snprintf(g.find(send)->values[2], 64, "4");
    const u32 on = g.add("CustomEvent", 0, 200)->id;
    std::snprintf(g.find(on)->values[0], 64, "ping");
    const u32 make = g.add("MakeVector", 200, 300)->id;
    const u32 move = g.add("AddOffset", 400, 200)->id;
    CHECK(g.connect(start, 0, send, 0) && g.connect(on, 0, move, 0) && g.connect(on, 1, make, 0) && g.connect(make, 0, move, 2));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    CHECK(std::strstr(lua.data(), "function ping(self, value)") != nullptr);
    write(kGraphLua, lua.data());
    HeapAllocator heap("events");
    {
        World w(heap);
        const Entity a = w.create("A"), b = w.create("B");
        std::snprintf(w.add<Script>(a)->script, sizeof(Script::script), "graph");
        std::snprintf(w.add<Script>(b)->script, sizeof(Script::script), "graph");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        // Each start broadcasts once, so both objects moved 4 m twice.
        CHECK(scripts.error_count() == 0 && w.get<Transform>(a)->position.x == 8.0f && w.get<Transform>(b)->position.x == 8.0f);
        char line[160];
        std::snprintf(line, sizeof line, "assert(lsd.call(%u, 'ping', 1) == 1 and lsd.implements(%u, 'ping') and not lsd.implements(%u, 'pong'))", a.value, a.value, a.value);
        CHECK(scripts.run(line));
        CHECK(w.get<Transform>(a)->position.x == 9.0f && w.get<Transform>(b)->position.x == 8.0f);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// Exposed properties: the script's defaults, then each object's own values, on self before on_start.
static void test_properties() {
    write(kPropsLua, R"(
properties = { speed = 5, label = "Door", locked = false }
function on_start(self)
  lsd.set_position(self.entity, self.speed, self.locked and 1 or 0, #self.label)
end
)");
    HeapAllocator heap("props");
    {
        World w(heap);
        const Entity a = w.create("A"), b = w.create("B");
        std::snprintf(w.add<Script>(a)->script, sizeof(Script::script), "props");
        Script* sb = w.add<Script>(b);
        std::snprintf(sb->script, sizeof(Script::script), "props");
        ScriptProperty p;
        std::snprintf(p.name, sizeof p.name, "speed");
        std::snprintf(p.value, sizeof p.value, "2.5");
        sb->properties.push_back(p);
        std::snprintf(p.name, sizeof p.name, "locked");
        std::snprintf(p.value, sizeof p.value, "true");
        sb->properties.push_back(p);
        std::snprintf(p.name, sizeof p.name, "label");
        std::snprintf(p.value, sizeof p.value, "Gate42");
        sb->properties.push_back(p);
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        const Vec3 pa = w.get<Transform>(a)->position, pb = w.get<Transform>(b)->position;
        CHECK(scripts.error_count() == 0 && pa.x == 5.0f && pa.y == 0.0f && pa.z == 4.0f);
        CHECK(pb.x == 2.5f && pb.y == 1.0f && pb.z == 6.0f);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// Editor-made enums and structs (data assets) from Lua.
static bool user_type_path(void*, const char* type, const char* name, char* out, usize capacity) {
    if (std::strcmp(type, "UserEnum") == 0 && std::strcmp(name, "E_Weapon") == 0) std::snprintf(out, capacity, "%s", LSD_TEST_TEMP_DIR "/E_Weapon.lsdasset");
    else if (std::strcmp(type, "UserStruct") == 0 && std::strcmp(name, "S_Item") == 0) std::snprintf(out, capacity, "%s", LSD_TEST_TEMP_DIR "/S_Item.lsdasset");
    else return false;
    return true;
}

static void test_user_types() {
    world::UserEnum e;
    for (const char* n : {"Sword", "Bow"}) {
        world::EnumEntry entry;
        std::snprintf(entry.name, sizeof entry.name, "%s", n);
        e.values.push_back(entry);
    }
    CHECK(asset::save_data_asset(LSD_TEST_TEMP_DIR "/E_Weapon.lsdasset", reflect::type_of<world::UserEnum>(), &e));
    world::UserStruct st;
    const char* fields[4][2] = {{"damage", "12.5"}, {"magic", "true"}, {"label", "Axe"}, {"offset", "1, 2, 3"}};
    for (u32 i = 0; i < 4; ++i) {
        world::StructField f;
        std::snprintf(f.name, sizeof f.name, "%s", fields[i][0]);
        std::snprintf(f.default_value, sizeof f.default_value, "%s", fields[i][1]);
        f.type = i;
        st.fields.push_back(f);
    }
    CHECK(asset::save_data_asset(LSD_TEST_TEMP_DIR "/S_Item.lsdasset", reflect::type_of<world::UserStruct>(), &st));
    HeapAllocator heap("types");
    {
        World w(heap);
        script::ScriptHost host;
        host.data_path = user_type_path;
        script::ScriptSystem scripts;
        scripts.start(w, host, path_of, nullptr);
        CHECK(scripts.run("local E = lsd.enum('E_Weapon') assert(E.Sword == 0 and E.Bow == 1 and E[1] == 'Bow' and lsd.enum('nope') == nil)"));
        CHECK(scripts.run("local a, b = lsd.struct('S_Item'), lsd.struct('S_Item') a.damage = 1 "
                          "assert(b.damage == 12.5 and b.magic == true and b.label == 'Axe' and b.offset.z == 3)"));  // each call: a fresh copy
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// Custom nodes registered from C++ (ScriptNode assets use the same call).
static void test_custom_nodes() {
    using namespace lsd::script;
    NodeDef twice{};
    twice.id = "Custom_Twice";
    twice.title = "Twice";
    twice.category = "Custom";
    twice.tooltip = "";
    twice.inputs[0] = {"Value", PinType::Float, "3"};
    twice.outputs[0] = {"Result", PinType::Float, ""};
    twice.output_code[0] = "($0 * 2)";
    register_node(twice);
    NodeDef move{};
    move.id = "Custom_MoveUp";
    move.title = "Move Up";
    move.category = "Custom";
    move.tooltip = "";
    move.inputs[0] = {"In", PinType::Exec, ""};
    move.inputs[1] = {"Height", PinType::Float, "1"};
    move.outputs[0] = {"Out", PinType::Exec, ""};
    move.code = "do local x, y, z = lsd.position(self.entity) lsd.set_position(self.entity, x, y, z + $1) end";
    register_node(move);
    CHECK(find_node_def("Custom_Twice") && find_node_def("Custom_Twice")->input_count == 1 && find_node_def("Custom_MoveUp")->output_count == 1);
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 up = g.add("Custom_MoveUp", 200, 0)->id;
    const u32 dbl = g.add("Custom_Twice", 0, 200)->id;
    std::snprintf(g.find(dbl)->values[0], 64, "4");
    CHECK(g.connect(start, 0, up, 0) && g.connect(dbl, 0, up, 1));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("nodes");
    {
        World w(heap);
        const Entity e = w.create("E");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "graph");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        CHECK(scripts.error_count() == 0 && w.get<Transform>(e)->position.z == 8.0f);
        scripts.stop();
    }
    clear_custom_nodes();
    CHECK(find_node_def("Custom_Twice") == nullptr && find_node_def("OnStart") != nullptr);
}

// The Lua debugger: a breakpoint stops with the locals, a step goes to the next line.
struct DebugLog {
    i32 lines[8];
    u32 count = 0;
    bool saw_local = false;
};
static script::DebugAction on_stop(void* user, const script::DebugStop& stop) {
    auto& log = *static_cast<DebugLog*>(user);
    if (log.count < 8) log.lines[log.count++] = stop.frames[0].line;
    for (u32 i = 0; i < stop.frames[0].local_count; ++i) {
        if (std::strcmp(stop.frames[0].locals[i].name, "a") == 0 && std::strcmp(stop.frames[0].locals[i].value, "41") == 0) log.saw_local = true;
    }
    return log.count == 1 ? script::DebugAction::StepOver : script::DebugAction::Continue;
}

static void test_debugger() {
    write(kDebugLua, "function on_start(self)\n  local a = 41\n  a = a + 1\n  self.answer = a\nend\n");
    HeapAllocator heap("debugger");
    {
        World w(heap);
        const Entity e = w.create("E");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "debugged");
        DebugLog log;
        script::ScriptSystem scripts;
        scripts.set_debugger(on_stop, &log);
        scripts.set_breakpoint(kDebugLua, 3, true);
        CHECK(scripts.has_breakpoint(kDebugLua, 3) && !scripts.has_breakpoint(kDebugLua, 2));
        scripts.start(w, {}, path_of, nullptr);
        CHECK(log.count == 2 && log.lines[0] == 3 && log.lines[1] == 4 && log.saw_local);  // stopped at 3, stepped to 4
        CHECK(scripts.run("assert(true)") && scripts.error_count() == 0);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// JavaScript breakpoints: line markers keep tricky code working (same result as without them), a
// breakpoint stops on its line with self's fields, a step goes to the next statement.
struct JsDebugLog {
    i32 lines[8];
    u32 count = 0;
    bool saw_self = false;
    bool top_frame_named = false;
};
static script::DebugAction on_js_stop(void* user, const script::DebugStop& stop) {
    auto& log = *static_cast<JsDebugLog*>(user);
    if (log.count < 8) log.lines[log.count++] = stop.frames[0].line;
    log.top_frame_named = log.top_frame_named || std::strcmp(stop.frames[0].function, "on_start") == 0;
    for (u32 i = 0; i < stop.frames[0].local_count; ++i) {
        if (std::strcmp(stop.frames[0].locals[i].name, "self.speed") == 0 && std::strcmp(stop.frames[0].locals[i].value, "5") == 0) log.saw_self = true;
    }
    return log.count == 1 ? script::DebugAction::StepOver : script::DebugAction::Continue;
}

static void test_js_debugger() {
    const char* lines[] = {
        "\"use strict\";",                                       // 1
        "var properties = { speed: 5 };",                         // 2
        "const table = {",                                        // 3
        "  a: 1,",                                                // 4
        "  b: [1, 2,",                                            // 5
        "      3],",                                              // 6
        "};",                                                     // 7
        "class Counter {",                                        // 8
        "  constructor() { this.n = 0; }",                        // 9
        "  add(k) {",                                             // 10
        "    this.n += k;",                                       // 11
        "    return this;",                                       // 12
        "  }",                                                    // 13
        "}",                                                      // 14
        "function on_start(self) {",                              // 15
        "  let total = 0;",                                       // 16
        "  const re = /a{2}\\/b/g;",                              // 17
        "  const s = `x ${ { k: 1 }.k } y",                       // 18
        "  z`;",                                                  // 19
        "  for (let i = 0; i < 3; i++) {",                        // 20
        "    total += i;",                                        // 21
        "  }",                                                    // 22
        "  if (total > 1)",                                       // 23
        "    total += 10;",                                       // 24
        "  else",                                                 // 25
        "    total -= 10;",                                       // 26
        "  switch (total) {",                                     // 27
        "    case 13:",                                           // 28
        "      total += 1;",                                      // 29
        "      break;",                                           // 30
        "    default:",                                           // 31
        "      total = -1;",                                      // 32
        "  }",                                                    // 33
        "  const g = () => {",                                    // 34
        "    return 3;",                                          // 35
        "  };",                                                   // 36
        "  const c = new Counter()",                              // 37
        "    .add(1)",                                            // 38
        "    .add(2);",                                           // 39
        "  // 14 + 3 + 3 + 9 + 3 + 100",                          // 40
        "  const result = total + c.n + table.b.length + s.length + g() + (re.test('aa/b') ? 100 : 0);",  // 41
        "  lsd.set_position(self.entity, result, 0, 0);",         // 42
        "}",                                                      // 43
    };
    std::string text;
    for (const char* l : lines) text += std::string(l) + "\n";
    write(LSD_TEST_TEMP_DIR "/debugged.js", text.c_str());
    HeapAllocator heap("js debugger");
    for (int debugged = 0; debugged < 2; ++debugged) {
        World w(heap);
        const Entity e = w.create("E");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "file:debugged.js");
        JsDebugLog log;
        script::ScriptSystem scripts;
        if (debugged) {
            scripts.set_debugger(on_js_stop, &log);
            scripts.set_breakpoint(LSD_TEST_TEMP_DIR "/debugged.js", 16, true);
        }
        scripts.start(w, {}, path_of, nullptr);
        CHECK(scripts.error_count() == 0);
        CHECK(w.get<Transform>(e)->position.x == 132.0f);  // the same answer with and without line markers
        if (debugged) {
            CHECK(log.count == 2 && log.lines[0] == 16 && log.lines[1] == 17);  // stopped at 16, stepped to 17
            CHECK(log.saw_self && log.top_frame_named);
        }
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

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

    // Typed columns and JSON export.
    char column[32];
    CHECK(asset::column_type("Damage:number", column, sizeof column) == asset::ColumnType::Number && std::strcmp(column, "Damage") == 0);
    CHECK(asset::column_type("Time:of:day", column, sizeof column) == asset::ColumnType::Auto && std::strcmp(column, "Time:of:day") == 0);
    CHECK(asset::cell_fits(asset::ColumnType::Int, "12") && !asset::cell_fits(asset::ColumnType::Int, "1.5") && !asset::cell_fits(asset::ColumnType::Number, "abc"));
    CHECK(asset::cell_fits(asset::ColumnType::Vector, "1, 2, 3") && !asset::cell_fits(asset::ColumnType::Vector, "1, 2") && asset::cell_fits(asset::ColumnType::Bool, ""));
    const char typed[] = "Name,Damage:number,Code:string,On:bool\nPistol,12.5,007,true\n";
    CHECK(t.parse(typed, sizeof typed - 1));
    asset::export_json(t, out);
    out.push_back('\0');
    CHECK(std::strstr(out.data(), "\"Pistol\": {\"Damage\": 12.5, \"Code\": \"007\", \"On\": true}") != nullptr);

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
        // Typed columns: "007" stays text, booleans and vectors read as such; a curve table interpolates.
        write(kTable, "Name,Code:string,Auto:bool,Aim:vector,0,10\nPistol,007,true,\"1,2,3\",5,25\n");
        std::filesystem::last_write_time(kTable, std::filesystem::last_write_time(kTable) + std::chrono::seconds(10));
        CHECK(scripts.run(R"(
local p = lsd.table("DT_Weapons").Pistol
assert(p.Code == "007" and p.Auto == true and p.Aim.y == 2)
assert(lsd.curve_table("DT_Weapons", "Pistol", 5) == 15 and lsd.curve_table("DT_Weapons", "Pistol", 99) == 25 and lsd.curve_table("DT_Weapons", "Nope", 1) == 0)
)"));
        // A Curve Vector gives three values.
        write(kCurve, R"({"type": "CurveVector", "data": {"x": [{"time": 0, "value": 1}], "y": [{"time": 0, "value": 2}], "z": [{"time": 0, "value": 3}]}})");
        std::filesystem::last_write_time(kCurve, std::filesystem::last_write_time(kCurve) + std::chrono::seconds(10));
        CHECK(scripts.run("local x, y, z = lsd.curve('C_Ramp', 0) assert(x == 1 and y == 2 and z == 3)"));
        // Save games round-trip through JSON; bad slot characters are dropped, functions refused.
        scripts.set_save_directory(LSD_TEST_TEMP_DIR "/SaveGames");
        CHECK(scripts.run(R"(
lsd.delete_save("Slot1")
assert(not lsd.has_save("Slot1") and lsd.load_game("Slot1") == nil)
assert(lsd.save_game("Slot1", {score = 1250.5, name = "Ana \"Q\"\nline", alive = true, items = {"sword", "key", 3}, pos = {x = 1, y = -2}}))
assert(lsd.has_save("Slot../1"))  -- the same file: "Slot1"
local s = lsd.load_game("Slot1")
assert(s.score == 1250.5 and s.name == "Ana \"Q\"\nline" and s.alive == true)
assert(#s.items == 3 and s.items[1] == "sword" and s.items[3] == 3 and s.pos.y == -2)
assert(not pcall(lsd.save_game, "Slot2", {f = print}))
assert(lsd.delete_save("Slot1") and not lsd.has_save("Slot1"))
)"));
        // Whole objects: Transform and a chosen component back on the objects by name; extra rides along.
        const world::Entity crate = w.create("Crate");
        w.create("Barrel");
        CHECK(scripts.run(R"(
local crate, barrel = lsd.find("Crate"), lsd.find("Barrel")
lsd.set_position(crate, 1, 2, 3)
lsd.set_rotation(crate, 0, 0, 90)
lsd.add(barrel, "RigidBody", {mass = 7})
assert(lsd.save_objects("World1", {crate, barrel}, {"Transform", "RigidBody"}, {score = 42}))
lsd.set_position(crate, 0, 0, 0)
lsd.set(barrel, "RigidBody", {mass = 1})
lsd.remove(crate, "RigidBody")
local extra, found = lsd.load_objects("World1")
assert(extra.score == 42 and found == 2)
local x, y, z = lsd.position(crate)
assert(x == 1 and y == 2 and z == 3 and select(3, lsd.rotation(crate)) == 90)
assert(lsd.get(barrel, "RigidBody").mass == 7 and not lsd.has(crate, "RigidBody"))  -- the crate had none when saved
assert(lsd.load_objects("Missing") == nil)
lsd.delete_save("World1")
)"));
        CHECK(w.alive(crate));
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// JavaScript scripts: the same events and lsd API as Lua, properties, timers, broadcasts both ways,
// errors kept to their script, and a reload that keeps self.
static void test_javascript() {
    write(LSD_TEST_TEMP_DIR "/door.js", R"(
var properties = { speed: 5, label: "Door" };
let opened = 0;
function on_start(self) {
  self.ticks = 0;
  const [x, y, z] = lsd.position(self.entity);
  lsd.set_position(self.entity, self.speed, self.label.length, z + 1);
  lsd.after(0.25, () => { self.fired = true; lsd.set_rotation(self.entity, 0, 0, 45); });
}
function on_update(self, dt) { self.ticks++; }
function ping(self, amount) { opened += amount; lsd.set(self.entity, "Transform", { scale: { x: opened } }); }
function probe(self) {
  try { lsd.position(12345); return false; } catch (e) { return lsd.get(self.entity, "Transform").scale.x > 0; }
}
)");
    write(LSD_TEST_TEMP_DIR "/caller.lua", "function on_start(self) lsd.broadcast('ping', 2) end\n");
    write(LSD_TEST_TEMP_DIR "/bad.js", "function on_update(self, dt) { null.x; }\n");
    HeapAllocator heap("javascript");
    {
        World w(heap);
        const Entity door = w.create("Door");
        std::snprintf(w.add<Script>(door)->script, sizeof(Script::script), "file:door.js");
        std::snprintf(w.add<Script>(w.create("Caller"))->script, sizeof(Script::script), "file:caller.lua");
        std::snprintf(w.add<Script>(w.create("Bad"))->script, sizeof(Script::script), "file:bad.js");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        const Vec3 p = w.get<Transform>(door)->position;
        CHECK(scripts.script_count() == 3 && scripts.error_count() == 0);
        CHECK(p.x == 5.0f && p.y == 4.0f && p.z == 1.0f);
        CHECK(w.get<Transform>(door)->scale.x == 2.0f);  // Lua's broadcast reached the JS function
        for (int i = 0; i < 3; ++i) scripts.update(0.1f);
        CHECK(w.get<Transform>(door)->rotation.z == 45.0f);  // the JS timer fired
        CHECK(scripts.error_count() == 1);                   // bad.js stopped, the door kept going
        char line[96];
        std::snprintf(line, sizeof line, "assert(lsd.call(%u, 'ping', 1) == 1)", door.value);
        CHECK(scripts.run(line) && w.get<Transform>(door)->scale.x == 3.0f);
        CHECK(scripts.call_task(door, "probe", 0.0f) == 1);  // a Lua error is a JS exception it can catch

        write(LSD_TEST_TEMP_DIR "/door.js", "function on_update(self, dt) { lsd.set_position(self.entity, self.ticks, self.fired ? 1 : 0, 0); }\n");
        std::filesystem::last_write_time(LSD_TEST_TEMP_DIR "/door.js", std::filesystem::last_write_time(LSD_TEST_TEMP_DIR "/door.js") + std::chrono::seconds(5));
        scripts.update(0.5f);
        CHECK(w.get<Transform>(door)->position.x == 3.0f && w.get<Transform>(door)->position.y == 1.0f);  // self kept
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
}

// JavaScript nodes: a .js file's `nodes` description becomes graph nodes calling its functions.
static void test_js_nodes() {
    using namespace lsd::script;
    write(LSD_TEST_TEMP_DIR "/lift.js", R"(
var nodes = {
  lift: { title: "Lift", category: "Moves", inputs: { target: "object", height: ["number", 2] }, output: "number" },
  twice: { pure: true, inputs: { value: "number" }, output: "number" },
  missing: { inputs: {} },
};
function lift(target, height) {
  const [x, y, z] = lsd.position(target);
  lsd.set_position(target, x, y, z + height);
  return z + height;
}
function twice(value) { return value * 2; }
)");
    CHECK(register_js_nodes(LSD_TEST_TEMP_DIR "/lift.js", "lift") == 2);  // `missing` has no function
    const NodeDef* lift = find_node_def("JS_lift_lift");
    CHECK(lift && lift->input_count == 3 && lift->output_count == 2 && std::strcmp(lift->inputs[1].default_value, "self") == 0);
    CHECK(find_node_def("JS_lift_twice") && is_pure(*find_node_def("JS_lift_twice")));
    Graph g(default_allocator());
    const u32 start = g.add("OnStart", 0, 0)->id;
    const u32 up = g.add("JS_lift_lift", 200, 0)->id;
    const u32 dbl = g.add("JS_lift_twice", 0, 200)->id;
    const u32 again = g.add("JS_lift_lift", 400, 0)->id;
    std::snprintf(g.find(dbl)->values[0], 64, "1.5");
    CHECK(g.connect(start, 0, up, 0) && g.connect(dbl, 0, up, 2) && g.connect(up, 0, again, 0) && g.connect(up, 1, again, 2));
    Array<char> lua(default_allocator());
    char error[256];
    CHECK(compile_graph(g, lua, error, sizeof error));
    lua.push_back('\0');
    write(kGraphLua, lua.data());
    HeapAllocator heap("js nodes");
    {
        World w(heap);
        const Entity e = w.create("E");
        std::snprintf(w.add<Script>(e)->script, sizeof(Script::script), "graph");
        script::ScriptSystem scripts;
        scripts.start(w, {}, path_of, nullptr);
        // Up by twice(1.5) = 3, then by the first node's result (3): z = 6.
        CHECK(scripts.error_count() == 0 && w.get<Transform>(e)->position.z == 6.0f);
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
    clear_custom_nodes();
}

int main() {
    test_javascript();
    test_js_nodes();
    test_native_nodes();
    test_native_components();
    test_data_table();
    test_graph();
    test_collapse_to_function();
    test_graph_variables();
    test_events();
    test_traces();
    test_forward_vector();
    test_native_behaviours();
    test_game_instance();
    test_functions();
    test_properties();
    test_user_types();
    test_custom_nodes();
    test_debugger();
    test_js_debugger();
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
        // Spawn and destroy many more than the 256 script slots: a destroyed object's slot is reused.
        CHECK(scripts.run("lsd.destroy(lsd.find('Spawned'))"));
        bool all_started = true;
        for (int i = 0; i < 600 && all_started; ++i) {
            CHECK(scripts.run("_G.e = lsd.spawn('Mover', 0, 0, 0) started = nil"));
            world::Entity spawned;
            for (u32 slot = 0; slot < w.slot_count(); ++slot) {
                const world::Entity e = w.entity_at_slot(slot);
                if (w.alive(e) && std::strcmp(w.name(e), "Spawned") == 0) spawned = e;
            }
            scripts.attach(spawned);
            all_started = scripts.run("assert(started == 'Spawned') lsd.destroy(e)");
        }
        CHECK(all_started && scripts.script_count() == before + 1);
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
