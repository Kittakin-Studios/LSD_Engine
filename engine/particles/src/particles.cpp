#include "lsd/particles/particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lsd::particles {

ParticleInstance::~ParticleInstance() {
    for (Emitter& e : emitters_) {
        if (e.particles) allocator_.deallocate(e.particles);
    }
}

f32 ParticleInstance::random() {
    seed_ ^= seed_ << 13;  // xorshift32
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return static_cast<f32>(seed_ >> 8) * (1.0f / 16777216.0f);
}

void ParticleInstance::restart() {
    for (Emitter& e : emitters_) {
        e.count = 0;
        e.time = e.spawn_carry = 0.0f;
        e.burst_done = e.done = false;
    }
}

bool ParticleInstance::finished() const {
    for (const Emitter& e : emitters_) {
        if (e.count || (e.capacity && !e.done)) return false;
    }
    return true;
}

void ParticleInstance::spawn(const ParticleEmitter& e, Emitter& s, const Mat4& world) {
    if (s.count == s.capacity) return;
    Particle& p = s.particles[s.count++];
    // Position in the shape.
    Vec3 local{};
    if (e.shape == 1) {  // sphere: uniform in the volume
        Vec3 d;
        do d = {random() * 2.0f - 1.0f, random() * 2.0f - 1.0f, random() * 2.0f - 1.0f};
        while (dot(d, d) > 1.0f);
        local = d * e.shape_size.x;
    } else if (e.shape == 2) {
        local = mul(Vec3{random() * 2.0f - 1.0f, random() * 2.0f - 1.0f, random() * 2.0f - 1.0f}, e.shape_size);
    }
    // Direction: uniform in the cone of `spread` degrees around `direction`.
    const f32 axis_length = length(e.direction);
    const Vec3 axis = axis_length > 1e-6f ? e.direction * (1.0f / axis_length) : Vec3{0, 0, 1};
    const f32 cos_max = std::cos(radians(e.spread < 180.0f ? e.spread : 180.0f));
    const f32 cos_t = 1.0f - random() * (1.0f - cos_max);
    const f32 sin_t = std::sqrt(std::fmax(0.0f, 1.0f - cos_t * cos_t));
    const f32 phi = random() * 6.2831853f;
    const Vec3 helper = std::fabs(axis.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    const Vec3 u = normalize(cross(helper, axis));
    const Vec3 v = cross(axis, u);
    const Vec3 dir = axis * cos_t + u * (sin_t * std::cos(phi)) + v * (sin_t * std::sin(phi));
    const Vec3 velocity = dir * (e.speed_min + (e.speed_max - e.speed_min) * random());
    p.position = e.local_space ? local : transform_point(world, local);
    p.velocity = e.local_space ? velocity : transform_vector(world, velocity);
    p.age = 0.0f;
    p.lifetime = e.lifetime_min + (e.lifetime_max - e.lifetime_min) * random();
    p.rotation = random() * 360.0f;
    p.spin = (random() * 2.0f - 1.0f) * e.spin;
}

void ParticleInstance::update(const ParticleSystem& system, const Mat4& world, f32 dt, f32 gravity, bool emitting, ParticleCollider* collider) {
    for (u32 i = 0; i < kMaxEmitters; ++i) {
        Emitter& s = emitters_[i];
        if (i >= system.emitters.count || !system.emitters[i].enabled) {
            s.count = 0;
            s.done = true;
            continue;
        }
        const ParticleEmitter& e = system.emitters[i];
        const u32 wanted = e.max_particles < 1 ? 1 : e.max_particles > kMaxParticles ? kMaxParticles : e.max_particles;
        if (s.capacity != wanted) {  // first use, or Max Particles edited: keep what fits
            auto* fresh = static_cast<Particle*>(allocator_.allocate(sizeof(Particle) * wanted, alignof(Particle)));
            s.count = s.count < wanted ? s.count : wanted;
            if (s.particles) {
                std::memcpy(fresh, s.particles, sizeof(Particle) * s.count);
                allocator_.deallocate(s.particles);
            }
            s.particles = fresh;
            s.capacity = wanted;
        }

        // Age, move, die.
        const Vec3 force = Vec3{0, 0, -gravity * e.gravity_scale} + e.acceleration;
        const Vec3 local_force = e.local_space ? transform_vector(affine_inverse(world), force) : force;
        const f32 keep = std::fmax(0.0f, 1.0f - e.drag * dt);
        for (u32 k = 0; k < s.count;) {
            Particle& p = s.particles[k];
            p.age += dt;
            if (p.age >= p.lifetime) {
                p = s.particles[--s.count];  // swap-remove: order does not matter
                continue;
            }
            p.velocity = (p.velocity + local_force * dt) * keep;
            const Vec3 from = p.position;
            p.position = p.position + p.velocity * dt;
            p.rotation += p.spin * dt;
            // Collision: the move from -> to against the floor plane or the world; then bounce or die.
            bool hit = false;
            Vec3 point, normal{0, 0, 1};
            if (e.collision == 1 && !e.local_space && from.z >= e.floor_height && p.position.z < e.floor_height) {
                const f32 t = (from.z - e.floor_height) / (from.z - p.position.z);
                point = from + (p.position - from) * t;
                hit = true;
            } else if (e.collision == 2 && !e.local_space && collider && collider->segment && collider->budget) {
                --collider->budget;
                hit = collider->segment(collider->user, from, p.position, point, normal);
            }
            if (hit) {
                if (e.kill_on_hit) {
                    p = s.particles[--s.count];
                    continue;
                }
                const f32 into = dot(p.velocity, normal);
                const Vec3 along = p.velocity - normal * into;
                p.velocity = along * (1.0f - e.friction) - normal * (into * e.bounce);
                p.position = point + normal * 0.01f;  // just off the surface: the next move starts outside it
            }
            ++k;
        }

        // Spawn: the burst at each start, then the rate.
        if (!emitting || s.done) continue;
        if (!s.burst_done) {
            for (u32 b = 0; b < e.burst; ++b) spawn(e, s, world);
            s.burst_done = true;
        }
        s.spawn_carry += e.spawn_rate * dt;
        while (s.spawn_carry >= 1.0f) {
            spawn(e, s, world);
            s.spawn_carry -= 1.0f;
        }
        s.time += dt;
        if (e.duration > 0.0f && s.time >= e.duration) {
            if (e.looping) {
                s.time -= e.duration;
                s.burst_done = false;
            } else {
                s.done = true;
            }
        }
    }
}

namespace {

// Where `t` falls between keys a and b of a time-sorted list: the pair and the blend between them.
template <typename Key, u32 N>
void bracket(const List<Key, N>& keys, f32 t, u32& a, u32& b, f32& blend) {
    i32 before = -1, after = -1;  // the latest key at or before t, the earliest after it (any order)
    for (u32 i = 0; i < keys.count; ++i) {
        const f32 k = keys[i].time;
        if (k <= t && (before < 0 || k >= keys[static_cast<u32>(before)].time)) before = static_cast<i32>(i);
        if (k > t && (after < 0 || k < keys[static_cast<u32>(after)].time)) after = static_cast<i32>(i);
    }
    a = static_cast<u32>(before >= 0 ? before : after);  // before the first / after the last key: hold it
    b = static_cast<u32>(after >= 0 ? after : before);
    blend = before >= 0 && after >= 0 ? (t - keys[a].time) / (keys[b].time - keys[a].time) : 0.0f;
}

} // namespace

f32 size_at(const ParticleEmitter& e, f32 t) {
    if (e.size_over_life.count == 0) return e.size_start + (e.size_end - e.size_start) * t;
    u32 a, b;
    f32 k;
    bracket(e.size_over_life, t, a, b, k);
    return e.size_start * (e.size_over_life[a].value + (e.size_over_life[b].value - e.size_over_life[a].value) * k);
}

LinearColor color_at(const ParticleEmitter& e, f32 t) {
    LinearColor from = e.color_start, to = e.color_end;
    f32 k = t;
    if (e.color_over_life.count) {
        u32 a, b;
        bracket(e.color_over_life, t, a, b, k);
        from = e.color_over_life[a].color;
        to = e.color_over_life[b].color;
    }
    return {from.r + (to.r - from.r) * k, from.g + (to.g - from.g) * k, from.b + (to.b - from.b) * k, from.a + (to.a - from.a) * k};
}

u32 ParticleInstance::build_sprites(const ParticleSystem& system, u32 emitter, const Mat4& world, Vec3 eye, Vec3 right, Vec3 up,
                                    rhi::ParticleVertex* out, u32 capacity) const {
    if (emitter >= system.emitters.count || emitter >= kMaxEmitters) return 0;
    const ParticleEmitter& e = system.emitters[emitter];
    const Emitter& s = emitters_[emitter];
    u32 n = s.count < capacity / 6 ? s.count : capacity / 6;
    if (!n) return 0;
    // Back to front for Alpha (Additive does not care about order).
    static thread_local u32 order[kMaxParticles];
    static thread_local f32 depth[kMaxParticles];
    for (u32 i = 0; i < n; ++i) {
        order[i] = i;
        const Vec3 p = e.local_space ? transform_point(world, s.particles[i].position) : s.particles[i].position;
        depth[i] = dot(p - eye, p - eye);
    }
    if (e.blend == 0) std::sort(order, order + n, [](u32 a, u32 b) { return depth[a] > depth[b]; });
    static const f32 corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (u32 i = 0; i < n; ++i) {
        const Particle& p = s.particles[order[i]];
        const f32 t = p.lifetime > 0.0f ? p.age / p.lifetime : 1.0f;
        const f32 half = 0.5f * size_at(e, t);
        const LinearColor lc = color_at(e, t);
        const f32 color[4] = {lc.r, lc.g, lc.b, lc.a};
        const Vec3 centre = e.local_space ? transform_point(world, p.position) : p.position;
        const f32 c = std::cos(radians(p.rotation)), sn = std::sin(radians(p.rotation));
        const Vec3 x = (right * c + up * sn) * half;
        const Vec3 y = (up * c - right * sn) * half;
        for (u32 k = 0; k < 6; ++k) {
            rhi::ParticleVertex& v = out[i * 6 + k];
            const Vec3 at = centre + x * corners[k][0] + y * corners[k][1];
            v.position[0] = at.x;
            v.position[1] = at.y;
            v.position[2] = at.z;
            std::memcpy(v.color, color, sizeof color);
            v.uv[0] = corners[k][0] * 0.5f + 0.5f;
            v.uv[1] = 0.5f - corners[k][1] * 0.5f;
        }
    }
    return n * 6;
}

} // namespace lsd::particles
