// AI tests: a navmesh over a floor with a wall in the middle; paths go round the wall and agents
// walk to their targets.
#include "lsd/ai/behavior_tree.h"
#include "lsd/ai/env_query.h"
#include "lsd/ai/smart_object.h"
#include "lsd/ai/state_tree.h"
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

    // Decorators and a service, each on its own small tree.
    auto fresh = [&](BtType type, f32 value, const char* key = "", const char* text = "") {
        t = BehaviorTree{};
        const u32 r = add(BtType::Root), d = add(type, key, value, text);
        link(r, d);
        return d;
    };
    // Loop 3 x a one-tick script = success on the third tick.
    const u32 deco1 = fresh(BtType::Loop, 3.0f);
    link(deco1, add(BtType::RunScript, "", 0, "bt_done"));
    BtRunner looper;
    agent.scripts = 0;
    for (int i = 0; i < 3; ++i) looper.tick(t, board, host, 0.1f);
    CHECK(agent.scripts == 3 && looper.active(ids, 16) == 3);  // root, loop, script: done (not running)
    // Blackboard Condition (is set): a running Wait is cut off the tick the key is cleared.
    const u32 cond = fresh(BtType::Condition, 0.0f, "Alert");
    t.nodes[cond].op = 4;
    link(cond, add(BtType::Wait, "", 10.0f));
    BtRunner guard;
    Blackboard alert;
    alert.set("Alert", 1.0f);
    guard.tick(t, alert, host, 0.1f);
    CHECK(guard.active(ids, 16) == 3);  // waiting under the condition
    alert.clear("Alert");
    guard.tick(t, alert, host, 0.1f);
    CHECK(guard.active(ids, 16) == 0);  // the condition failed (so did the root): the Wait was stopped
    // Cooldown 1 s: the script runs, then is refused until a second has passed.
    const u32 deco2 = fresh(BtType::Cooldown, 1.0f);
    link(deco2, add(BtType::RunScript, "", 0, "bt_done"));
    BtRunner cooled;
    agent.scripts = 0;
    for (int i = 0; i < 5; ++i) cooled.tick(t, board, host, 0.3f);  // at 0.3 runs; 0.6, 0.9, 1.2 refused; 1.5 runs again
    CHECK(agent.scripts == 2);
    // Time Limit 0.5 s on a 10 s Wait: failed (and stopped) after the limit; Force Success hides a failure.
    const u32 deco3 = fresh(BtType::TimeLimit, 0.5f);
    link(deco3, add(BtType::Wait, "", 10.0f));
    BtRunner limited;
    for (int i = 0; i < 3; ++i) limited.tick(t, board, host, 0.2f);  // 0.6 s: over the limit (the next tick starts it again)
    CHECK(limited.active(ids, 16) == 0);  // failed, the Wait stopped
    const u32 deco4 = fresh(BtType::ForceSuccess, 0.0f);
    link(deco4, add(BtType::IsSet, "Nothing"));
    BtRunner forced;
    forced.tick(t, board, host, 0.1f);
    CHECK(forced.active(ids, 16) == 2);  // root and the decorator succeeded; the condition itself failed
    // Service every 0.5 s over a long Wait: on entry, then at 0.5 and 1.0 s.
    const u32 deco5 = fresh(BtType::Service, 0.5f, "", "bt_update");
    link(deco5, add(BtType::Wait, "", 10.0f));
    BtRunner serviced;
    agent.scripts = 0;
    for (int i = 0; i < 11; ++i) serviced.tick(t, board, host, 0.1f);
    CHECK(agent.scripts == 3);
}

// A Blackboard asset's keys: defaults written, writes checked and made to fit their type.
static void blackboard_schema_tests() {
    using namespace ai;
    BlackboardSchema schema;
    auto key = [&](const char* name, BbType type, f32 a, f32 b = 0.0f, f32 c = 0.0f) {
        BbKey& k = schema.keys[schema.count++];
        std::snprintf(k.name, sizeof k.name, "%s", name);
        k.type = type;
        k.value[0] = a, k.value[1] = b, k.value[2] = c;
    };
    key("Alert", BbType::Bool, 1.0f);
    key("Ammo", BbType::Int, 6.4f);
    key("Home", BbType::Vector, 1.0f, 2.0f, 3.0f);
    key("Target", BbType::Object, 0.0f);
    Blackboard board;
    apply_defaults(schema, board);
    CHECK(board.is_set("Alert") && board.find("Ammo") && board.find("Ammo")->value[0] == 6.0f && !board.find("Target"));
    CHECK(board.find("Home") && board.find("Home")->vector && board.find("Home")->value[2] == 3.0f);
    const f32 two_and_a_bit[3] = {2.7f, 0, 0}, place[3] = {5, 6, 7};
    CHECK(!typed_set(schema, board, "Ammo", two_and_a_bit, false) && board.find("Ammo")->value[0] == 3.0f);  // an Int is rounded
    CHECK(!typed_set(schema, board, "Alert", two_and_a_bit, false) && board.find("Alert")->value[0] == 1.0f);  // a Bool is 0 or 1
    CHECK(typed_set(schema, board, "Home", two_and_a_bit, false) != nullptr);  // a location wanted
    CHECK(typed_set(schema, board, "Ammo", place, true) != nullptr);           // a number wanted
    CHECK(typed_set(schema, board, "Typo", place, true) != nullptr && !board.find("Typo"));
    BlackboardSchema untyped;  // no asset: anything goes
    CHECK(!typed_set(untyped, board, "Typo", place, true) && board.find("Typo"));
}

// Nav links: two floors 2 m apart have no path between them until a link joins them.
static void nav_link_tests() {
    std::vector<f32> verts;
    std::vector<u32> idx;
    add_box(verts, idx, {-10, -5, -0.2f}, {-1, 5, 0});
    add_box(verts, idx, {1, -5, -0.2f}, {10, 5, 0});
    Vec3 path[32];
    ai::NavMesh apart;
    CHECK(apart.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-11, -6, -1}, {11, 6, 3}, {}));
    u32 n = apart.find_path({-5, 0, 0}, {5, 0, 0}, path, 32);
    CHECK(n == 0 || path[n - 1].x < 0.0f);  // stops on its own side
    ai::NavLink link;
    link.start = {-1.8f, 0, 0};
    link.end = {1.8f, 0, 0};
    ai::NavMesh joined;
    CHECK(joined.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-11, -6, -1}, {11, 6, 3}, {}, nullptr, 0, &link, 1));
    n = joined.find_path({-5, 0, 0}, {5, 0, 0}, path, 32);
    CHECK(n >= 2 && std::fabs(path[n - 1].x - 5.0f) < 0.3f);  // across the gap
    n = joined.find_path({5, 0, 0}, {-5, 0, 0}, path, 32);
    CHECK(n >= 2 && std::fabs(path[n - 1].x + 5.0f) < 0.3f);  // both ways
    link.both_ways = false;
    ai::NavMesh one_way;
    CHECK(one_way.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-11, -6, -1}, {11, 6, 3}, {}, nullptr, 0, &link, 1));
    n = one_way.find_path({5, 0, 0}, {-5, 0, 0}, path, 32);
    CHECK(n == 0 || path[n - 1].x > 0.0f);  // no way back up
    Vec3 lines[4096];
    const u32 drawn = joined.debug_lines(lines, 4096);
    bool link_drawn = false;
    for (u32 i = 0; i + 1 < drawn; i += 2) link_drawn |= std::fabs(lines[i].x + 1.8f) < 0.3f && std::fabs(lines[i + 1].x - 1.8f) < 0.3f;
    CHECK(link_drawn);
}

// Tiled rebuilds: building the same level again rebuilds nothing; a crate added in one corner rebuilds only the
// few tiles under it, and paths still cross the whole floor.
static void nav_tile_tests() {
    std::vector<f32> verts;
    std::vector<u32> idx;
    add_box(verts, idx, {-20, -20, -0.2f}, {20, 20, 0});  // 40 m: 5 x 5 tiles of 64 cells at 0.15 m
    ai::NavMesh nav;
    auto build = [&] { return nav.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-21, -21, -1}, {21, 21, 4}, {}); };
    CHECK(build());
    const u32 all = nav.last_rebuilt_tiles();
    CHECK(all >= 25);
    CHECK(build() && nav.last_rebuilt_tiles() == 0);  // nothing changed
    add_box(verts, idx, {15, 15, 0}, {16, 16, 1});       // a crate in one corner
    CHECK(build() && nav.last_rebuilt_tiles() > 0 && nav.last_rebuilt_tiles() <= 4);
    Vec3 path[64];
    const u32 n = nav.find_path({-18, -18, 0}, {18, 12, 0}, path, 64);
    CHECK(n >= 2 && std::fabs(path[n - 1].x - 18) < 0.3f && std::fabs(path[n - 1].y - 12) < 0.3f);  // across tiles
    Vec3 p;
    CHECK(nav.nearest_point({15.5f, 15.5f, 0.0f}, p) && std::fabs(p.x - 15.5f) + std::fabs(p.y - 15.5f) > 0.4f);  // not under the crate
}

// A State Tree: Patrol (wait 1 s, then Completed -> Rest), Rest (Timer 0.5 s -> Patrol), and from either a
// Condition (Alert is set) -> Alarm, whose script task fails -> back to Patrol.
static void state_tree_tests() {
    using namespace lsd::ai;
    StateTree t;
    t.count = 3;
    auto task = [](StState& s, BtType type, f32 value, const char* text = "") {
        BtNode& n = s.tasks[s.task_count++];
        n.type = type;
        n.value = value;
        std::snprintf(n.text, sizeof n.text, "%s", text);
    };
    auto go = [](StState& s, StTrigger when, u32 target, f32 value = 0, const char* key = "") {
        StTransition& tr = s.transitions[s.transition_count++];
        tr.trigger = when;
        tr.target = target;
        tr.value = value;
        tr.op = 4;
        std::snprintf(tr.key, sizeof tr.key, "%s", key);
    };
    go(t.states[0], StTrigger::Condition, 2, 0, "Alert");
    task(t.states[0], BtType::Wait, 1.0f);
    go(t.states[0], StTrigger::Completed, 1);
    go(t.states[1], StTrigger::Condition, 2, 0, "Alert");
    go(t.states[1], StTrigger::Timer, 0, 0.5f);
    task(t.states[2], BtType::RunScript, 0, "alarm");
    go(t.states[2], StTrigger::Succeeded, 1);
    go(t.states[2], StTrigger::Failed, 0);

    int alarms = 0;
    BtHost host;
    host.user = &alarms;
    host.run_script = [](void* u, const char*, f32) { ++*static_cast<int*>(u); return 0; };
    Blackboard board;
    StRunner runner;
    const f32 dt = 0.1f;
    runner.tick(t, board, host, dt);
    CHECK(runner.state() == 0);
    for (int i = 0; i < 10; ++i) runner.tick(t, board, host, dt);
    CHECK(runner.state() == 1);  // waited 1 s, then Completed
    for (int i = 0; i < 6; ++i) runner.tick(t, board, host, dt);
    CHECK(runner.state() == 0);  // the timer sent it back
    board.set("Alert", 1.0f);
    runner.tick(t, board, host, dt);
    CHECK(runner.state() == 2);  // the condition interrupted the wait
    board.clear("Alert");
    runner.tick(t, board, host, dt);
    CHECK(alarms == 1 && runner.state() == 0);  // the script failed: Failed -> Patrol

    // pick_transition alone: order wins, and Succeeded / Failed only once the tasks are done.
    CHECK(pick_transition(t.states[2], board, false, BtStatus::Running, 5.0f) == -1);
    CHECK(pick_transition(t.states[2], board, true, BtStatus::Success, 0.0f) == 0);
    CHECK(pick_transition(t.states[2], board, true, BtStatus::Failure, 0.0f) == 1);
}

// Smart Objects: one slot each, two agents; the Use Smart Object task claims, walks, uses, releases.
static void smart_object_tests() {
    using namespace lsd::ai;
    SmartClaims claims;
    const i32 first = claims.claim(7, 0, 1);
    CHECK(first >= 0 && claims.taken(7, 0) && claims.claim(7, 0, 2) == -1);
    const Vec3 spots[2] = {{2, 0, 0}, {6, 0, 0}};
    bool taken[2] = {true, false};
    CHECK(nearest_free(spots, taken, 2, {0, 0, 0}, 10.0f) == 1);  // the near one is taken
    CHECK(nearest_free(spots, taken, 2, {0, 0, 0}, 5.0f) == -1);  // the free one is too far
    claims.release(first);
    CHECK(!claims.taken(7, 0) && !claims.get(first));
    claims.claim(7, 1, 3);
    claims.release_user(3);
    CHECK(!claims.taken(7, 1));

    struct User {
        Vec3 at{0, 0, 0}, target{0, 0, 0};
        bool walking = false;
        SmartClaims claims;
        int uses = 0;
    } user;
    BehaviorTree t;
    t.count = 2;
    t.nodes[0].type = BtType::Root;
    t.nodes[0].child_count = 1;
    t.nodes[0].children[0] = 1;
    t.nodes[1].type = BtType::UseSmartObject;
    t.nodes[1].value = 10.0f;
    BtHost host;
    host.user = &user;
    host.move_to = [](void* u, Vec3 to, f32) { auto& a = *static_cast<User*>(u); a.target = to; a.walking = true; return true; };
    host.moving = [](void* u) { return static_cast<User*>(u)->walking; };
    host.stop = [](void* u) { static_cast<User*>(u)->walking = false; };
    host.position = [](void* u) { return static_cast<User*>(u)->at; };
    host.claim_smart_object = [](void* u, const char*, f32, Vec3& slot) { slot = {3, 0, 0}; return static_cast<User*>(u)->claims.claim(9, 0, 1); };
    host.use_smart_object = [](void* u, i32, f32 seconds) { ++static_cast<User*>(u)->uses; return seconds >= 1.0f ? 1 : 2; };
    host.release_smart_object = [](void* u, i32 id) { static_cast<User*>(u)->claims.release(id); };
    BtRunner runner;
    Blackboard board;
    BtStatus r = BtStatus::Running;
    int frames = 0;
    for (; frames < 200 && r == BtStatus::Running; ++frames) {
        r = runner.tick(t, board, host, 0.1f);
        if (frames == 2) CHECK(user.claims.taken(9, 0) && user.uses == 0);  // claimed, still walking
        if (user.walking) {  // 1 m a tick
            const Vec3 d = user.target - user.at;
            if (length(d) <= 1.0f) user.at = user.target, user.walking = false;
            else user.at = user.at + d * (1.0f / length(d));
        }
    }
    CHECK(r == BtStatus::Success && user.uses >= 10 && std::fabs(user.at.x - 3) < 0.01f);
    CHECK(!user.claims.taken(9, 0));  // released when done
}

// Environment query: a ring around the querier; points the wall hides from the target are dropped, the rest
// prefer being near the target.
static void env_query_tests() {
    using namespace lsd::ai;
    EnvQuery q;
    q.rings = 1;
    q.points_per_ring = 8;
    q.radius = 4.0f;
    q.on_navmesh = false;
    q.tests[0].type = QueryTestType::Visible;
    q.tests[0].filter = true;
    q.tests[0].score = false;
    q.tests[1].type = QueryTestType::Distance;
    q.tests[1].weight = -1.0f;  // nearer the target is better
    q.test_count = 2;
    QueryHost host;
    host.visible = [](void*, Vec3 from, Vec3) { return from.y > -1.0f; };  // a wall hides everything south
    QueryPoint points[kMaxQueryPoints];
    u32 count = 0;
    const i32 best = run_query(q, host, {0, 0, 0}, {1, 0, 0}, {10, 0, 0}, points, kMaxQueryPoints, count);
    CHECK(count == 8);
    CHECK(best == 0 && std::fabs(points[0].at.x - 4) < 1e-3f);  // due east, nearest the target
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) kept += points[i].kept ? 1u : 0u;
    CHECK(kept == 5);  // the three southern points are hidden
    // Facing on a grid: the point straight ahead wins; a filter can drop everything.
    q.generator = QueryGenerator::Grid;
    q.radius = 2.0f;
    q.spacing = 1.0f;
    q.tests[0] = QueryTest{};
    q.tests[0].type = QueryTestType::Facing;
    q.test_count = 1;
    const i32 ahead = run_query(q, host, {0, 0, 0}, {0, 1, 0}, {0, 0, 0}, points, kMaxQueryPoints, count);
    CHECK(count == 25 && ahead >= 0 && points[ahead].at.y > 0.5f && std::fabs(points[ahead].at.x) < 1e-3f);
    q.tests[0].type = QueryTestType::Distance;
    q.tests[0].filter = true;
    q.tests[0].min = 100.0f;
    CHECK(run_query(q, host, {0, 0, 0}, {0, 1, 0}, {0, 0, 0}, points, kMaxQueryPoints, count) == -1);
}

// Two towns 3 km apart under one bounds box (Nav Mesh Bounds merged): only the tiles with floors count, so it builds
// (the box is about 300 x 300 tiles; it used to be refused above 128 x 128) and each town has its paths.
static void nav_far_apart_tests() {
    std::vector<f32> verts;
    std::vector<u32> idx;
    add_box(verts, idx, {-20, -20, -0.2f}, {20, 20, 0});
    add_box(verts, idx, {2980, 2980, -0.2f}, {3020, 3020, 0});
    ai::NavMesh nav;
    CHECK(nav.build(verts.data(), static_cast<u32>(verts.size() / 3), idx.data(), static_cast<u32>(idx.size() / 3), {-25, -25, -1}, {3025, 3025, 3}, {}));
    Vec3 path[32];
    u32 n = nav.find_path({-10, 0, 0}, {10, 0, 0}, path, 32);
    CHECK(n >= 2 && std::fabs(path[n - 1].x - 10.0f) < 0.3f);
    n = nav.find_path({2990, 3000, 0}, {3010, 3000, 0}, path, 32);
    CHECK(n >= 2 && std::fabs(path[n - 1].x - 3010.0f) < 0.3f);
}

int main() {
    nav_far_apart_tests();
    state_tree_tests();
    smart_object_tests();
    env_query_tests();
    bt_tests();
    nav_tile_tests();
    blackboard_schema_tests();
    nav_link_tests();
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

    // A Nav Modifier cutout (turned 45 degrees) over the floor's middle: paths go round it.
    {
        std::vector<f32> floor_verts;
        std::vector<u32> floor_idx;
        add_box(floor_verts, floor_idx, {-10, -10, -0.2f}, {10, 10, 0});
        ai::NavCutout cut;
        cut.corners[0] = {0, -3, 0};
        cut.corners[1] = {3, 0, 0};
        cut.corners[2] = {0, 3, 0};
        cut.corners[3] = {-3, 0, 0};
        cut.z_min = -1.0f;
        cut.z_max = 2.0f;
        ai::NavMesh holed;
        CHECK(holed.build(floor_verts.data(), static_cast<u32>(floor_verts.size() / 3), floor_idx.data(), static_cast<u32>(floor_idx.size() / 3), {-11, -11, -1}, {11, 11, 4},
                          {}, &cut, 1));
        Vec3 on;
        CHECK(holed.nearest_point({0, 0, 0}, on) && std::fabs(on.x) + std::fabs(on.y) > 2.5f);  // the centre is cut out
        Vec3 route[32];
        const u32 m = holed.find_path({-6, 0, 0}, {6, 0, 0}, route, 32);
        f32 off_axis = 0;
        for (u32 i = 0; i < m; ++i) off_axis = std::fmax(off_axis, std::fabs(route[i].y));
        CHECK(m >= 3 && off_axis > 2.5f);  // round the diamond, not through it
    }

    if (g_failures == 0) std::printf("All AI tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
