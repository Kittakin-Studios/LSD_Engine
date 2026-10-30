// Particle tests: spawning (burst, rate, cap, duration), motion, death and sprite building.
#include "lsd/particles/particles.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

int main() {
    Allocator& heap = default_allocator();
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
    if (g_failures == 0) std::printf("All particle tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
