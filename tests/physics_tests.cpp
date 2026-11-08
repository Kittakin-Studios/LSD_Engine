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

// A weak weld snaps under its weight; a cone joint keeps its swing inside the cone; a sprung slider holds.
static void test_joint_extras() {
    PhysicsWorld w;
    const f32 dt = 1.0f / 60.0f;
    const u32 sign = w.add_body(box({0, 0, 10}, {0.3f, 0.3f, 0.3f}, Motion::Dynamic));  // 10 kg: ~98 N of weight
    JointDesc weld;
    weld.type = JointType::Fixed;
    weld.body_a = sign;
    weld.break_force = 20.0f;
    const u32 weld_index = w.add_joint(weld);

    const u32 lamp = w.add_body(box({12, 0, 10}, {0.1f, 0.1f, 0.1f}, Motion::Dynamic));  // 2 m out along +X from its pivot
    JointDesc cone;
    cone.type = JointType::Cone;
    cone.body_a = lamp;
    cone.pivot = {10, 0, 10};
    cone.axis = {1, 0, 0};  // swing measured from +X
    cone.cone_angle = 30.0f;
    w.add_joint(cone);

    const u32 bouncer = w.add_body(box({20, 0, 10}, {0.3f, 0.3f, 0.3f}, Motion::Dynamic));
    JointDesc spring;
    spring.type = JointType::Slider;
    spring.body_a = bouncer;
    spring.pivot = {20, 0, 10};
    spring.limited = true;
    spring.limit_min = 0.0f;
    spring.limit_max = 0.0f;
    spring.spring_frequency = 2.0f;
    w.add_joint(spring);

    u32 broken[4];
    u32 broken_count = 0;
    for (int i = 0; i < 180; ++i) {
        w.step(dt);
        broken_count += w.take_broken_joints(broken + broken_count, 4 - broken_count);
    }
    CHECK(broken_count == 1 && broken[0] == weld_index);
    CHECK(centre(w, sign).z < 5.0f);  // fell once the weld snapped
    const Vec3 hang = centre(w, lamp) - Vec3{10, 0, 10};
    CHECK(std::atan2(-hang.z, hang.x) < 0.8f);  // held near the 30 degree cone (it still swings a little past it), not hanging straight down
    CHECK(centre(w, bouncer).z < 10.0f && centre(w, bouncer).z > 8.0f);  // sagged on the spring, not dropped
}

// Mesh collision (a ramp's triangles), a convex hull falling onto it, and debris passing through bodies.
static void test_shapes() {
    PhysicsWorld w;
    w.add_body(box({0, 0, -0.5f}, {50, 50, 0.5f}, Motion::Static));  // floor
    // A wedge: 2 m long, rising to 1 m at +X; its sloped top is what a box test would miss.
    const Vec3 ramp_points[6] = {{-1, -1, 0}, {1, -1, 0}, {1, -1, 1}, {-1, 1, 0}, {1, 1, 0}, {1, 1, 1}};
    const u32 ramp_tris[] = {0, 1, 2, 3, 5, 4, 0, 5, 3, 0, 2, 5, 1, 5, 2, 1, 4, 5, 0, 4, 1, 0, 3, 4};  // counter-clockwise from outside
    BodyDesc ramp;
    ramp.shape = Shape::Mesh;
    ramp.transform = translation({0, 0, 0});
    ramp.points = ramp_points;
    ramp.point_count = 6;
    ramp.indices = ramp_tris;
    ramp.index_count = sizeof ramp_tris / sizeof ramp_tris[0];
    w.add_body(ramp);
    RayHit hit;
    CHECK(w.raycast({0, 0, 5}, {0, 0, -1}, 10, hit) && std::fabs(hit.point.z - 0.5f) < 0.05f && hit.normal.x < -0.4f && hit.normal.z > 0.8f);  // the slope, not a box top at 1

    // A convex hull (a small pyramid) dropped above the floor comes to rest on it.
    const Vec3 pyramid[5] = {{-0.3f, -0.3f, -0.2f}, {0.3f, -0.3f, -0.2f}, {0.3f, 0.3f, -0.2f}, {-0.3f, 0.3f, -0.2f}, {0, 0, 0.3f}};
    BodyDesc rock;
    rock.shape = Shape::Convex;
    rock.motion = Motion::Dynamic;
    rock.transform = translation({5, 0, 2});
    rock.points = pyramid;
    rock.point_count = 5;
    const u32 r = w.add_body(rock);
    // Debris above a dynamic crate falls through it to the floor.
    const u32 crate = w.add_body(box({10, 0, 0.5f}, {0.5f, 0.5f, 0.5f}, Motion::Dynamic));
    BodyDesc pebble = box({10, 0, 3}, {0.1f, 0.1f, 0.1f}, Motion::Dynamic);
    pebble.debris = true;
    const u32 p = w.add_body(pebble);
    for (int i = 0; i < 240; ++i) w.step(1.0f / 60.0f);
    CHECK(std::fabs(centre(w, r).z - 0.2f) < 0.1f);   // resting on its base
    CHECK(centre(w, p).z < 0.3f);                      // through the crate, on the floor
    CHECK(std::fabs(centre(w, crate).z - 0.5f) < 0.05f);  // the crate never noticed
}

// A four-wheeled car (2 x 4 m box, 1500 kg, rear drive): it settles on its suspension, drives
// forward under throttle, turns right when steered right, and the handbrake stops it.
static void test_vehicle() {
    PhysicsWorld w;
    w.add_body(box({0, 0, -0.5f}, {500, 500, 0.5f}, Motion::Static));
    BodyDesc car = box({0, 0, 1.0f}, {2.0f, 1.0f, 0.4f}, Motion::Dynamic);
    car.mass = 1500.0f;
    car.centre_of_mass_offset = {0, 0, -0.4f};
    const u32 body = w.add_body(car);
    VehicleDesc v;
    v.body = body;
    const f32 xs[2] = {1.4f, -1.4f}, ys[2] = {0.8f, -0.8f};
    for (u32 i = 0; i < 4; ++i) {
        WheelDesc& wheel = v.wheels[v.wheel_count++];
        wheel.position = {xs[i / 2], ys[i % 2], -0.3f};
        wheel.max_steer_degrees = i < 2 ? 30.0f : 0.0f;  // front steers
        wheel.driven = i >= 2;                           // rear drives
        wheel.handbrake = i >= 2;
    }
    const u32 vehicle = w.add_vehicle(v);
    for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);  // settle
    const Vec3 rest = centre(w, body);
    const VehicleState parked = w.vehicle_state(vehicle);
    std::printf("  vehicle at rest: z %.2f, %u wheels down\n", static_cast<double>(rest.z), parked.wheels_on_ground);
    CHECK(parked.wheels_on_ground == 4 && rest.z > 0.4f && rest.z < 1.2f && std::fabs(parked.speed) < 0.1f);
    const Mat4 wheel = w.wheel_transform(vehicle, 0);
    CHECK(wheel.at(0, 3) > 1.0f && wheel.at(1, 3) > 0.5f && wheel.at(2, 3) > 0.2f && wheel.at(2, 3) < 0.5f);  // front left, at about its radius
    w.drive_vehicle(vehicle, 1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
    const Vec3 ahead = centre(w, body);
    const VehicleState driving = w.vehicle_state(vehicle);
    std::printf("  after 3 s of throttle: x %.1f m, %.1f m/s, gear %d, %.0f rpm\n", static_cast<double>(ahead.x), static_cast<double>(driving.speed), driving.gear,
                static_cast<double>(driving.rpm));
    CHECK(ahead.x > 5.0f && driving.speed > 3.0f && driving.gear >= 1 && std::fabs(ahead.y) < 0.5f);
    w.drive_vehicle(vehicle, 0.5f, 1.0f, 0.0f, 0.0f);  // steer right: Y goes negative (Z up, X forward)
    for (int i = 0; i < 90; ++i) w.step(1.0f / 60.0f);
    CHECK(centre(w, body).y < ahead.y - 0.5f);
    w.drive_vehicle(vehicle, 0.0f, 0.0f, 1.0f, 1.0f);
    for (int i = 0; i < 300; ++i) w.step(1.0f / 60.0f);
    CHECK(std::fabs(w.vehicle_state(vehicle).speed) < 0.3f);
    // Another machine drives it (multiplayer): kinematic, its engine ignored; then ours again.
    w.set_body_kinematic(body, true);
    const Vec3 held = centre(w, body);
    w.drive_vehicle(vehicle, 1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);
    CHECK(length(centre(w, body) - held) < 0.01f);
    w.set_body_kinematic(body, false);
    w.drive_vehicle(vehicle, 1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
    CHECK(length(centre(w, body) - held) > 1.0f);
    w.remove_body(body);  // takes its vehicle with it
    w.drive_vehicle(vehicle, 1.0f, 0.0f, 0.0f, 0.0f);
    w.step(1.0f / 60.0f);
}

// A one-way platform: the character jumps up through it from below and then stands on its top.
static void test_one_way() {
    PhysicsWorld world;
    world.add_body(box({0, 0, -0.5f}, {20, 20, 0.5f}, Motion::Static));  // floor, top at z = 0
    BodyDesc slab = box({0, 0, 1.6f}, {3, 3, 0.1f}, Motion::Static);      // a thin platform, top at z = 1.7
    slab.one_way = true;
    world.add_body(slab);
    CharacterDesc desc;
    desc.feet = {0, 0, 0.05f};
    world.create_character(desc);
    for (int i = 0; i < 30; ++i) {
        world.move_character({}, false, 8.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    CHECK(world.character_feet().z < 0.2f);  // stands on the floor, under the platform
    world.move_character({}, true, 8.0f, 1.0f / 60.0f);  // a jump that clears 1.7 m
    world.step(1.0f / 60.0f);
    f32 highest = 0.0f;
    for (int i = 0; i < 150; ++i) {
        world.move_character({}, false, 8.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
        highest = world.character_feet().z > highest ? world.character_feet().z : highest;
    }
    CHECK(highest > 1.8f);                             // went up through it
    CHECK(std::fabs(world.character_feet().z - 1.7f) < 0.1f && world.character_on_ground());  // and landed on top
}

int main() {
    test_one_way();
    test_vehicle();
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

        // Sweeps: a 0.5 m sphere down onto the crate stops with its centre 0.5 m above the top.
        QueryShape ball;
        ball.radius = 0.5f;
        CHECK(world.sweep(ball, {0, 3, 10}, {0, 3, -5}, hit) && hit.body == crate && std::fabs(hit.distance - 8.5f) < 0.1f && hit.normal.z > 0.9f);
        QueryShape crate_box;
        crate_box.kind = QueryShape::Box;
        crate_box.half_extents = {0.25f, 0.25f, 0.25f};
        CHECK(world.sweep(crate_box, {0, 0, 1}, {9, 0, 1}, hit) && hit.body == 1 && std::fabs(hit.distance - 4.25f) < 0.1f && hit.normal.x < -0.9f);
        QueryShape pill;
        pill.kind = QueryShape::Capsule;
        pill.radius = 0.3f;
        pill.half_height = 0.6f;
        CHECK(!world.sweep(pill, {0, 0, 2}, {0, 0, 6}, hit));  // up into nothing
        // Overlaps: a sphere around the crate finds it (and the floor under it), not the far wall.
        u32 found[8];
        const u32 n = world.overlap(ball, {0, 3, 1.2f}, found, 8);
        bool has_crate = false, has_wall = false;
        for (u32 k = 0; k < n; ++k) {
            has_crate = has_crate || found[k] == crate;
            has_wall = has_wall || found[k] == 1;
        }
        CHECK(has_crate && !has_wall);
        CHECK(world.overlap(ball, {0, 0, 30}, found, 8) == 0);
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

    // Crouch under a 1.4 m ceiling: shrinking works, standing up there is refused, and works again in the open.
    world.add_body(box({-10, 0, 1.5f}, {1, 1, 0.1f}, Motion::Static));
    world.teleport_character({-14, 0, 0.05f});
    CHECK(world.set_character_height(1.1f));
    world.teleport_character({-10, 0, 0.05f});
    for (int i = 0; i < 30; ++i) {
        world.move_character({}, false, 5.0f, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    CHECK(!world.set_character_height(1.8f));
    world.teleport_character({-14, 0, 0.05f});
    CHECK(world.set_character_height(1.8f));
    // Swimming: no gravity, velocity.z lifts.
    const f32 before = world.character_feet().z;
    for (int i = 0; i < 30; ++i) {
        world.move_character({0, 0, 2}, false, 0.0f, 1.0f / 60.0f, true);
        world.step(1.0f / 60.0f);
    }
    CHECK(world.character_feet().z > before + 0.8f);

    test_joints();
    test_shapes();
    test_joint_extras();
    if (g_failures == 0) std::printf("All physics tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
