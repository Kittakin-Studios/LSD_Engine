// Script tests: a Lua file moves its object, finds others, reads tags and input, survives errors
// and reloads when the file changes.
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

static void write(const char* path, const char* text) {
    FILE* f = std::fopen(path, "wb");
    std::fputs(text, f);
    std::fclose(f);
}

static bool path_of(void*, const char* guid, char* out, usize capacity) {
    std::snprintf(out, capacity, "%s", std::strcmp(guid, "mover") == 0 ? kMover : kBroken);
    return true;
}

static Vec3 input(void*, const char* action) { return std::strcmp(action, "IA_Move") == 0 ? Vec3{0.5f, 1.0f, 0} : Vec3{}; }
static bool pressed(void*, const char* action) { return std::strcmp(action, "IA_Jump") == 0; }

int main() {
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
        scripts.stop();
    }
    CHECK(heap.allocated_bytes() == 0);
    if (g_failures == 0) std::printf("All script tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
