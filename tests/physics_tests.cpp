// Physics tests: a box falls and rests on the floor; the character stands, walks, is stopped by a
// wall, climbs a step and jumps (Z up).
#include "lsd/physics/physics.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::physics;

static BodyDesc box(Vec3 centre, Vec3 half, Motion motion) {
    BodyDesc d;
    d.half_extents = half;
    d.transform = translation(centre);
    d.motion = motion;
    return d;
}

static Vec3 centre(const PhysicsWorld& w, u32 body) {
    const Mat4 m = w.body_transform(body);
    return {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
}

// Pendulum on a ball joint, a hinged flap with limits, a slider stopped by its limit, a box welded
// to the world and a motor-driven wheel (no floor: everything else would just fall).
static void test_joints() {
    PhysicsWorld w;
    const f32 dt = 1.0f / 60.0f;
    const u32 bob = w.add_body(box({2, 0, 10}, {0.2f, 0.2f, 0.2f}, Motion::Dynamic));
    JointDesc ball;
    ball.type = JointType::Ball;
    ball.body_a = bob;
    ball.pivot = {0, 0, 10};  // 2 m to the side: it swings down
    w.add_joint(ball);

    const u32 flap = w.add_body(box({11, 0, 10}, {1, 0.1f, 0.1f}, Motion::Dynamic));
    JointDesc hinge;
    hinge.body_a = flap;
    hinge.pivot = {10, 0, 10};
    hinge.axis = {0, 1, 0};  // swings in the XZ plane
    hinge.limited = true;
    hinge.limit_min = -30.0f;
    hinge.limit_max = 30.0f;
    w.add_joint(hinge);

    const u32 sled = w.add_body(box({20, 0, 10}, {0.3f, 0.3f, 0.3f}, Motion::Dynamic));
    JointDesc slider;
    slider.type = JointType::Slider;
    slider.body_a = sled;
    slider.pivot = {20, 0, 10};
    slider.axis = {0, 0, 1};
    slider.limited = true;
    slider.limit_min = -1.0f;
    slider.limit_max = 0.0f;
    w.add_joint(slider);

    const u32 welded = w.add_body(box({30, 0, 10}, {0.3f, 0.3f, 0.3f}, Motion::Dynamic));
    JointDesc weld;
    weld.type = JointType::Fixed;
    weld.body_a = welded;
    w.add_joint(weld);

    const u32 wheel = w.add_body(box({40, 0, 10}, {0.5f, 0.5f, 0.1f}, Motion::Dynamic));
    JointDesc motor;
    motor.body_a = wheel;
    motor.pivot = {40, 0, 10};
    motor.axis = {0, 0, 1};
    motor.motor_speed = 90.0f;
    w.add_joint(motor);
    CHECK(w.joint_count() == 5);

    f32 lowest_flap = 10.0f, lowest_bob = 10.0f;
    for (int i = 0; i < 120; ++i) {
        w.step(dt);
        const Vec3 b = centre(w, bob);
        CHECK(std::fabs(length(b - Vec3{0, 0, 10}) - 2.0f) < 0.05f);  // stays on its 2 m rope
        lowest_flap = std::fmin(lowest_flap, centre(w, flap).z);
        lowest_bob = std::fmin(lowest_bob, b.z);
    }
    CHECK(lowest_bob < 8.1f);                             // it swung down to the bottom of its rope
    CHECK(lowest_flap > 10.0f - std::sin(0.6f) && lowest_flap < 9.6f);  // 30 deg limit (a little give): the centre drops ~0.5 m
    CHECK(std::fabs(centre(w, sled).z - 9.0f) < 0.05f && std::fabs(centre(w, sled).x - 20.0f) < 0.01f);  // slid down 1 m and stopped
    CHECK(std::fabs(centre(w, welded).z - 10.0f) < 0.01f);  // held in place
    const Mat4 spun = w.body_transform(wheel);
    const f32 turned = std::atan2(spun.at(1, 0), spun.at(0, 0));  // 2 s at 90 deg/s: half a turn (wraps to +-pi)
    CHECK(std::fabs(std::fabs(turned) - 3.14159f) < 0.3f && std::fabs(centre(w, wheel).z - 10.0f) < 0.05f);

    // A kinematic platform moved 3 m along X over a second carries a box resting on it.
    const u32 platform = w.add_body(box({0, 20, 1}, {2, 2, 0.1f}, Motion::Kinematic));
    const u32 rider = w.add_body(box({0, 20, 1.4f}, {0.3f, 0.3f, 0.3f}, Motion::Dynamic));
    for (int i = 0; i < 30; ++i) w.step(dt);  // settle
    for (int i = 1; i <= 60; ++i) {
        w.move_kinematic(platform, translation({3.0f * static_cast<f32>(i) / 60.0f, 20, 1}), dt);
        w.step(dt);
    }
    CHECK(std::fabs(centre(w, platform).x - 3.0f) < 0.01f);
    CHECK(centre(w, rider).x > 1.5f && centre(w, rider).z > 1.0f);  // carried along (slipping a little on the sudden start), still on top

    w.remove_body(bob);  // its joint goes with it, no dangling pointer in Jolt
    w.step(dt);
}

int main() {
    PhysicsWorld world;
    world.add_body(box({0, 0, -0.5f}, {50, 50, 0.5f}, Motion::Static));        // floor, top at z = 0
    world.add_body(box({5, 0, 1.0f}, {0.5f, 5, 1.0f}, Motion::Static));        // wall at x = 4.5..5.5
    world.add_body(box({-3, 0, 0.15f}, {0.5f, 2, 0.15f}, Motion::Static));     // step 0.3 high at x = -3.5..-2.5
    const u32 crate = world.add_body(box({0, 3, 5}, {0.5f, 0.5f, 0.5f}, Motion::Dynamic));

    for (int i = 0; i < 180; ++i) world.step(1.0f / 60.0f);
    const Mat4 rest = world.body_transform(crate);
    {  // the crate landed: one new contact with the floor (body 0), reported once
        Contact contacts[64];
        const u32 n = world.take_contacts(contacts, 64);
        bool floor = false;
        for (u32 i = 0; i < n; ++i) floor |= (contacts[i].a == crate && contacts[i].b == 0) || (contacts[i].a == 0 && contacts[i].b == crate);
        CHECK(floor);
        world.step(1.0f / 60.0f);
        CHECK(world.take_contacts(contacts, 64) == 0);  // still touching: not new
    }
    {  // rays: down onto the crate's top, sideways into the wall, up into nothing
        RayHit hit;
        CHECK(world.raycast({0, 3, 10}, {0, 0, -1}, 100, hit) && hit.body == crate && std::fabs(hit.point.z - 1.0f) < 0.05f && hit.normal.z > 0.9f);
        CHECK(world.raycast({0, 0, 1}, {2, 0, 0}, 100, hit) && hit.body == 1 && std::fabs(hit.distance - 4.5f) < 0.05f && hit.normal.x < -0.9f);
        CHECK(!world.raycast({0, 0, 1}, {0, 0, 1}, 100, hit));
        CHECK(!world.raycast({0, 0, 1}, {2, 0, 0}, 3, hit));  // wall beyond reach
    }
    CHECK(std::fabs(rest.at(2, 3) - 0.5f) < 0.05f);  // centre half a metre above the floor

    CharacterDesc desc;
    desc.feet = {0, 0, 0.5f};
    world.create_character(desc);
    for (int i = 0; i < 60; ++i) {
        world.move_character({}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    CHECK(world.character_on_ground() && std::fabs(world.character_feet().z) < 0.05f);  // fell onto the floor

    for (int i = 0; i < 180; ++i) {  // walk +X into the wall
        world.move_character({4, 0, 0}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    const Vec3 at_wall = world.character_feet();
    {  // the player bumped the wall (body 1)
        Contact contacts[64];
        const u32 n = world.take_contacts(contacts, 64);
        bool wall = false;
        for (u32 i = 0; i < n; ++i) wall |= contacts[i].a == 1 && contacts[i].b == kCharacterBody;
        CHECK(wall);
    }
    CHECK(at_wall.x > 3.9f && at_wall.x < 4.2f);  // capsule radius 0.35 from the wall face at 4.5

    for (int i = 0; i < 180; ++i) {  // walk -X onto the step
        world.move_character({-4, 0, 0}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    const Vec3 on_step = world.character_feet();
    CHECK(on_step.x < -2.6f);
    world.teleport_character({-3, 0, 1});
    for (int i = 0; i < 60; ++i) {
        world.move_character({}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    CHECK(std::fabs(world.character_feet().z - 0.3f) < 0.05f);  // standing on top of the step

    world.move_character({}, true, 5.0f, 1.0f / 60.0f);  // jump
    world.step(1.0f / 60.0f);
    for (int i = 0; i < 10; ++i) {
        world.move_character({}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    CHECK(world.character_feet().z > 0.8f && !world.character_on_ground());

    test_joints();
    if (g_failures == 0) std::printf("All physics tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
