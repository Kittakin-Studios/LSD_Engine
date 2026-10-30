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

int main() {
    PhysicsWorld world;
    world.add_body(box({0, 0, -0.5f}, {50, 50, 0.5f}, Motion::Static));        // floor, top at z = 0
    world.add_body(box({5, 0, 1.0f}, {0.5f, 5, 1.0f}, Motion::Static));        // wall at x = 4.5..5.5
    world.add_body(box({-3, 0, 0.15f}, {0.5f, 2, 0.15f}, Motion::Static));     // step 0.3 high at x = -3.5..-2.5
    const u32 crate = world.add_body(box({0, 3, 5}, {0.5f, 0.5f, 0.5f}, Motion::Dynamic));

    for (int i = 0; i < 180; ++i) world.step(1.0f / 60.0f);
    const Mat4 rest = world.body_transform(crate);
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

    if (g_failures == 0) std::printf("All physics tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
