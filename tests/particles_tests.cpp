// Particle tests: spawning (burst, rate, cap, duration), motion, death and sprite building.
#include "lsd/particles/particles.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

// Collision: a floor they bounce on (losing speed), kill on hit, and the World test through a callback.
static bool slab_at_one(void*, Vec3 from, Vec3 to, Vec3& point, Vec3& normal) {  // a floor at z = 1
    if (!(from.z >= 1.0f && to.z < 1.0f)) return false;
    const f32 t = (from.z - 1.0f) / (from.z - to.z);
    point = from + (to - from) * t;
    normal = {0, 0, 1};
    return true;
}

static void test_collision(Allocator& heap) {
    particles::ParticleSystem system;
    system.emitters.count = 1;
    particles::ParticleEmitter& e = system.emitters[0];
    e.burst = 1;
    e.spawn_rate = 0.0f;
    e.lifetime_min = e.lifetime_max = 100.0f;
    e.speed_min = e.speed_max = 0.0f;
    e.gravity_scale = 1.0f;
    e.collision = 1;
    e.floor_height = 0.0f;
    e.bounce = 0.5f;
    const Mat4 at = translation({0, 0, 2});
    {
        particles::ParticleInstance fx(heap);
        f32 lowest = 10.0f, best_rise = 0.0f;
        for (int i = 0; i < 300; ++i) {
            fx.update(system, at, 1.0f / 60.0f, 9.81f, i == 0);
            const particles::Particle& p = fx.particles(0)[0];
            lowest = std::fmin(lowest, p.position.z);
            if (i > 60) best_rise = std::fmax(best_rise, p.position.z);
        }
        CHECK(fx.count(0) == 1 && lowest >= 0.0f);           // never under the floor
        CHECK(best_rise > 0.1f && best_rise < 1.0f);           // bounced back up, but lower than it fell from
    }
    {
        e.kill_on_hit = true;
        particles::ParticleInstance fx(heap);
        for (int i = 0; i < 120; ++i) fx.update(system, at, 1.0f / 60.0f, 9.81f, i == 0);
        CHECK(fx.count(0) == 0);  // died on the floor
    }
    {
        e.kill_on_hit = false;
        e.collision = 2;  // the world: here a slab at z = 1, through the callback
        particles::ParticleCollider collider;
        collider.segment = slab_at_one;
        particles::ParticleInstance fx(heap);
        f32 lowest = 10.0f;
        for (int i = 0; i < 120; ++i) {
            collider.budget = 1024;
            fx.update(system, at, 1.0f / 60.0f, 9.81f, i == 0, &collider);
            lowest = std::fmin(lowest, fx.particles(0)[0].position.z);
        }
        CHECK(lowest >= 1.0f);
        particles::ParticleInstance free_fall(heap);
        for (int i = 0; i < 120; ++i) free_fall.update(system, at, 1.0f / 60.0f, 9.81f, i == 0);  // no collider: falls through
        CHECK(free_fall.particles(0)[0].position.z < 0.0f);
    }
}

// Ribbons: one strip through the particles, a segment (6 vertices) between each pair, oldest first.
static void test_ribbon(Allocator& heap) {
    particles::ParticleSystem system;
    system.emitters.count = 1;
    particles::ParticleEmitter& e = system.emitters[0];
    e.spawn_rate = 20.0f;
    e.burst = 0;
    e.lifetime_min = e.lifetime_max = 5.0f;
    e.speed_min = e.speed_max = 0.0f;
    e.renderer = 2;
    e.size_start = e.size_end = 0.5f;
    particles::ParticleInstance fx(heap);
    for (int i = 0; i < 10; ++i) fx.update(system, translation({static_cast<f32>(i), 0, 0}), 0.05f, 9.81f, true);  // the emitter moves along X
    const u32 count = fx.count(0);
    static rhi::ParticleVertex vertices[6 * 64];
    const u32 n = fx.build_ribbon(system, 0, Mat4{}, {0, -10, 0}, vertices, 6 * 64);
    CHECK(count >= 5 && n == (count - 1) * 6);
    // Facing the camera (along Y): the strip is 0.5 m wide in Z, centred on the particles.
    CHECK(std::fabs(std::fabs(vertices[0].position[2] - vertices[1].position[2]) - 0.5f) < 1e-3f && std::fabs(vertices[0].position[1]) < 1e-4f);
    CHECK(vertices[0].position[0] < vertices[2].position[0]);  // oldest (smaller X) first
}

// Module toggles (Niagara's checkboxes): a switched-off module runs as if it were not there; its settings stay.
static void test_modules(Allocator& heap) {
    particles::ParticleSystem system;
    system.emitters.count = 1;
    particles::ParticleEmitter& e = system.emitters[0];
    e.burst = 1;
    e.spawn_rate = 0.0f;
    e.lifetime_min = e.lifetime_max = 100.0f;
    e.speed_min = e.speed_max = 0.0f;
    e.gravity_scale = 1.0f;
    e.color_start = {1, 0, 0, 1};
    e.color_end = {0, 0, 1, 0};
    e.disabled_modules = particles::kModuleForces | particles::kModuleColor;
    particles::ParticleInstance fx(heap);
    for (int i = 0; i < 60; ++i) fx.update(system, translation({0, 0, 2}), 1.0f / 60.0f, 9.81f, i == 0);
    CHECK(fx.count(0) == 1 && std::fabs(fx.particles(0)[0].position.z - 2.0f) < 1e-4f);  // no gravity
    const LinearColor c = particles::color_at(particles::active_modules(e), 0.9f);
    CHECK(c.r == 1.0f && c.b == 0.0f && c.a == 1.0f);  // the start colour all its life
    CHECK(e.gravity_scale == 1.0f && e.color_end.b == 1.0f);  // the settings are kept
    e.disabled_modules = 0;
    for (int i = 0; i < 60; ++i) fx.update(system, translation({0, 0, 2}), 1.0f / 60.0f, 9.81f, false);
    CHECK(fx.particles(0)[0].position.z < 1.0f);  // back on: it falls
}

int main() {
    Allocator& heap = default_allocator();
    test_collision(heap);
    test_ribbon(heap);
    test_modules(heap);
    particles::ParticleSystem system;
    system.emitters.count = 1;
    particles::ParticleEmitter& e = system.emitters[0];
    e.spawn_rate = 100.0f;
    e.burst = 10;
    e.lifetime_min = e.lifetime_max = 0.5f;
    e.speed_min = e.speed_max = 0.0f;
    e.max_particles = 1000;
    const Mat4 at = translation({0, 0, 5});

    {  // Burst + rate; everything dies after its life.
        particles::ParticleInstance fx(heap);
        fx.update(system, at, 0.1f, 9.81f, true);
        CHECK(fx.count(0) == 20);  // 10 burst + 100/s x 0.1 s
        CHECK(std::fabs(fx.particles(0)[0].position.z - 5.0f) < 1e-4f);  // born at the object (world space)
        fx.update(system, at, 0.1f, 9.81f, false);  // stops emitting
        CHECK(fx.count(0) == 20);
        fx.update(system, at, 0.5f, 9.81f, false);
        CHECK(fx.count(0) == 0 && !fx.finished());  // looping forever: still "alive"
    }
    {  // The cap, then a non-looping duration that finishes.
        particles::ParticleEmitter cap = e;
        cap.max_particles = 15;
        cap.duration = 0.2f;
        cap.looping = false;
        particles::ParticleSystem s2;
        s2.emitters.count = 1;
        s2.emitters[0] = cap;
        particles::ParticleInstance fx(heap);
        fx.update(s2, at, 0.1f, 9.81f, true);
        CHECK(fx.count(0) == 15);
        for (int i = 0; i < 20; ++i) fx.update(s2, at, 0.1f, 9.81f, true);
        CHECK(fx.count(0) == 0 && fx.finished());
        fx.restart();
        CHECK(!fx.finished());
    }
    {  // Gravity pulls down, speed in the direction, local space follows the object.
        particles::ParticleEmitter fall = e;
        fall.burst = 1;
        fall.spawn_rate = 0.0f;
        fall.lifetime_min = fall.lifetime_max = 10.0f;
        fall.gravity_scale = 1.0f;
        fall.speed_min = fall.speed_max = 2.0f;
        fall.direction = {1, 0, 0};
        fall.spread = 0.0f;
        particles::ParticleSystem s3;
        s3.emitters.count = 1;
        s3.emitters[0] = fall;
        particles::ParticleInstance fx(heap);
        fx.update(s3, at, 0.0f, 10.0f, true);
        for (int i = 0; i < 10; ++i) fx.update(s3, at, 0.1f, 10.0f, true);
        const particles::Particle& p = fx.particles(0)[0];
        CHECK(std::fabs(p.position.x - 2.0f) < 1e-3f);          // 2 m/s for 1 s
        CHECK(p.position.z < 5.0f - 4.0f && p.position.z > 5.0f - 6.0f);  // about 5 m down (Euler steps)

        rhi::ParticleVertex v[12];
        const u32 n = fx.build_sprites(s3, 0, at, {0, -10, 0}, {1, 0, 0}, {0, 0, 1}, v, 12);
        CHECK(n == 6);
        CHECK(fx.build_sprites(s3, 0, at, {0, -10, 0}, {1, 0, 0}, {0, 0, 1}, v, 5) == 0);  // no room for a whole sprite
    }
    {  // Over-life keys: size puffs up then shrinks (keys out of order on purpose); colour through three keys.
        particles::ParticleEmitter k;
        k.size_start = 2.0f;
        k.size_over_life.count = 3;
        k.size_over_life[0] = {1.0f, 0.0f};
        k.size_over_life[1] = {0.0f, 0.5f};
        k.size_over_life[2] = {0.25f, 1.5f};
        CHECK(std::fabs(particles::size_at(k, 0.0f) - 1.0f) < 1e-5f);    // 0.5 x 2
        CHECK(std::fabs(particles::size_at(k, 0.25f) - 3.0f) < 1e-5f);   // the peak
        CHECK(std::fabs(particles::size_at(k, 0.625f) - 1.5f) < 1e-5f);  // half-way down from 1.5 to 0
        CHECK(std::fabs(particles::size_at(k, 1.0f)) < 1e-5f);
        k.color_over_life.count = 3;
        k.color_over_life[0] = {0.0f, {1, 1, 1, 1}};
        k.color_over_life[1] = {0.5f, {1, 0.5f, 0, 1}};
        k.color_over_life[2] = {1.0f, {0.2f, 0.2f, 0.2f, 0}};
        const LinearColor mid = particles::color_at(k, 0.25f), late = particles::color_at(k, 0.75f);
        CHECK(std::fabs(mid.g - 0.75f) < 1e-5f && std::fabs(mid.b - 0.5f) < 1e-5f);
        CHECK(std::fabs(late.r - 0.6f) < 1e-5f && std::fabs(late.a - 0.5f) < 1e-5f);
        particles::ParticleEmitter plain;  // no keys: start -> end as before
        CHECK(std::fabs(particles::size_at(plain, 0.5f) - 0.3f) < 1e-5f && std::fabs(particles::color_at(plain, 0.5f).a - 0.5f) < 1e-5f);
    }
    if (g_failures == 0) std::printf("All particle tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
