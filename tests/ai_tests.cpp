// AI tests: a navmesh over a floor with a wall in the middle; paths go round the wall and agents
// walk to their targets.
#include "lsd/ai/behavior_tree.h"
#include "lsd/ai/navigation.h"
#include <cstring>

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

// An axis-aligned box as 8 corners and 12 triangles.
static void add_box(std::vector<f32>& v, std::vector<u32>& idx, Vec3 lo, Vec3 hi) {
    const u32 base = static_cast<u32>(v.size() / 3);
    for (int c = 0; c < 8; ++c) {
        v.push_back((c & 1) ? hi.x : lo.x);
        v.push_back((c & 2) ? hi.y : lo.y);
        v.push_back((c & 4) ? hi.z : lo.z);
    }
    // Outward faces (counter-clockwise seen from outside, Z up).
    static const u32 faces[12][3] = {{4, 5, 7}, {4, 7, 6}, {0, 2, 3}, {0, 3, 1}, {0, 1, 5}, {0, 5, 4},
                                     {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    for (const auto& f : faces) {
        for (u32 k : f) idx.push_back(base + k);
    }
}

// A pretend agent for the behaviour tree: walks 4 m/s straight at its target.
struct FakeAgent {
    Vec3 at{0, 0, 0}, target{0, 0, 0};
    bool walking = false;
    int scripts = 0;
};

static void bt_tests() {
    using namespace lsd::ai;
    BehaviorTree t;
    auto add = [&](BtType type, const char* key = "", f32 value = 0, const char* text = "") {
        BtNode& n = t.nodes[t.count];
        n.type = type;
        n.id = 100 + t.count;
        std::snprintf(n.key, sizeof n.key, "%s", key);
        std::snprintf(n.text, sizeof n.text, "%s", text);
        n.value = value;
        return t.count++;
    };
    auto link = [&](u32 parent, u32 child) { t.nodes[parent].children[t.nodes[parent].child_count++] = child; };
    // Root -> Selector [ Sequence(CanSeePlayer?, Move To PlayerLocation), Sequence(Random Patrol, Move To Patrol, Wait 1, Run bt_done) ]
    const u32 root = add(BtType::Root), sel = add(BtType::Selector);
    const u32 chase = add(BtType::Sequence), see = add(BtType::IsSet, "CanSeePlayer"), go = add(BtType::MoveTo, "PlayerLocation", 1.0f);
    const u32 patrol = add(BtType::Sequence), pick = add(BtType::RandomLocation, "Patrol", 5.0f), walk = add(BtType::MoveTo, "Patrol", 0.5f);
    const u32 wait = add(BtType::Wait, "", 1.0f), done = add(BtType::RunScript, "", 0, "bt_done");
    link(root, sel);
    link(sel, chase);
    link(chase, see);
    link(chase, go);
    link(sel, patrol);
    link(patrol, pick);
    link(patrol, walk);
    link(patrol, wait);
    link(patrol, done);

    FakeAgent agent;
    BtHost host;
    host.user = &agent;
    host.move_to = [](void* u, Vec3 to, f32) { auto& a = *static_cast<FakeAgent*>(u); a.target = to; a.walking = true; return true; };
    host.moving = [](void* u) { return static_cast<FakeAgent*>(u)->walking; };
    host.stop = [](void* u) { static_cast<FakeAgent*>(u)->walking = false; };
    host.position = [](void* u) { return static_cast<FakeAgent*>(u)->at; };
    host.random_point = [](void*, Vec3 around, f32 radius, Vec3& out) { out = around + Vec3{radius, 0, 0}; return true; };
    host.run_script = [](void* u, const char* f, f32) { ++static_cast<FakeAgent*>(u)->scripts; return std::strcmp(f, "bt_done") == 0 ? 1 : -1; };
    auto step = [&](FakeAgent& a, f32 dt) {
        if (!a.walking) return;
        const Vec3 d = a.target - a.at;
        const f32 len = lsd::length(d);
        if (len <= 4.0f * dt) { a.at = a.target; a.walking = false; }
        else a.at = a.at + d * (4.0f * dt / len);
    };

    Blackboard board;
    BtRunner runner;
    u32 ids[16];
    // Patrol: pick a point 5 m away, walk there (about 1.25 s), wait 1 s, run the script, repeat.
    for (int f = 0; f < 60 * 3; ++f) {
        runner.tick(t, board, host, 1.0f / 60.0f);
        step(agent, 1.0f / 60.0f);
    }
    CHECK(agent.scripts == 1);
    CHECK(agent.at.x > 5.0f);  // reached the first point, now on the next leg
    const u32 n = runner.active(ids, 16);
    CHECK(n >= 3 && ids[0] == 100 + root);  // root, selector, patrol sequence, (move or wait)
    // Seeing the player interrupts the patrol: the chase branch runs and the agent heads for the player.
    board.set("CanSeePlayer", 1.0f);
    board.set("PlayerLocation", Vec3{-10, 0, 0});
    runner.tick(t, board, host, 1.0f / 60.0f);
    CHECK(agent.walking && agent.target.x == -10.0f);
    for (int f = 0; f < 60 * 5; ++f) {
        runner.tick(t, board, host, 1.0f / 60.0f);
        step(agent, 1.0f / 60.0f);
    }
    CHECK(agent.at.x < -8.9f);  // within the 1 m acceptance
    // Out of sight: back to patrolling.
    board.clear("CanSeePlayer");
    runner.tick(t, board, host, 1.0f / 60.0f);
    CHECK(board.find("Patrol") && agent.walking);
    (void)go;
    (void)walk;
    (void)wait;
    (void)see;
    (void)pick;
    (void)done;
    (void)chase;
    (void)patrol;
}

int main() {
    bt_tests();
    std::vector<f32> verts;
    std::vector<u32> idx;
    add_box(verts, idx, {-10, -10, -0.2f}, {10, 10, 0});          // floor, top at z = 0
    add_box(verts, idx, {-0.5f, -6, 0}, {0.5f, 6, 3});            // a wall across the middle, gaps at both ends
    ai::NavMesh nav;
    CHECK(nav.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-11, -11, -1}, {11, 11, 4}, {}));
    CHECK(nav.valid());

    Vec3 p;
    CHECK(nav.nearest_point({3, 2, 1}, p) && std::fabs(p.z) < 0.3f && std::fabs(p.x - 3) < 0.2f);
    CHECK(!nav.nearest_point({0, 0, 3.5f}, p) || p.z > 2.5f || std::fabs(p.x) > 0.5f);  // not inside the wall

    Vec3 path[32];
    const u32 n = nav.find_path({-5, 0, 0}, {5, 0, 0}, path, 32);
    CHECK(n >= 3);  // round the wall, not through it
    f32 length = 0, widest = 0;
    for (u32 i = 1; i < n; ++i) length += lsd::length(path[i] - path[i - 1]);
    for (u32 i = 0; i < n; ++i) widest = std::fmax(widest, std::fabs(path[i].y));
    CHECK(length > 10.5f && widest > 6.0f);
    CHECK(n && std::fabs(path[n - 1].x - 5) < 0.2f && std::fabs(path[n - 1].y) < 0.2f);

    u32 lines = 0;
    std::vector<Vec3> debug(4096);
    lines = nav.debug_lines(debug.data(), 4096);
    CHECK(lines >= 6 && lines % 2 == 0);

    // An agent walks round the wall to the far side.
    const i32 agent = nav.add_agent({-5, 0, 0}, 0.4f, 1.8f, 4.0f);
    CHECK(agent >= 0);
    CHECK(nav.move_agent(agent, {5, 0, 0}));
    CHECK(nav.agent_moving(agent));
    f32 crossed_wall_y = 0;
    for (int frame = 0; frame < 60 * 10; ++frame) {
        nav.update(1.0f / 60.0f);
        const Vec3 at = nav.agent_position(agent);
        if (std::fabs(at.x) < 0.6f) crossed_wall_y = std::fmax(crossed_wall_y, std::fabs(at.y));
    }
    const Vec3 end = nav.agent_position(agent);
    CHECK(std::fabs(end.x - 5) < 0.6f && std::fabs(end.y) < 0.6f);
    CHECK(crossed_wall_y > 5.5f);  // went round the end of the wall
    CHECK(!nav.agent_moving(agent));

    if (g_failures == 0) std::printf("All AI tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
