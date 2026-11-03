#pragma once
// Particle systems (a data asset, "ParticleSystem"): Unreal's Niagara System, simulated on the CPU.
// A system is a stack of emitters. Each spawns particles (a rate and / or a burst) from a shape,
// moves them with velocity, gravity, wind and drag, and blends their colour and size from start to
// end over their life. They draw as camera-facing sprites: build_sprites -> rhi::Device::draw_particles.
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/math.h"
#include "lsd/reflect/color.h"
#include "lsd/reflect/reflect.h"
#include "lsd/rhi/rhi.h"

namespace lsd::particles {

// A point on an over-life curve: at `time` (0 = birth, 1 = death) the value is `value`; straight
// lines between points, the first / last value held before / after them.
LSD_STRUCT()
struct LifeKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = birth, 1 = death") f32 time = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "Times Size Start") f32 value = 1.0f;
};

LSD_STRUCT()
struct LifeColorKey {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = birth, 1 = death") f32 time = 0.0f;
    LSD_PROPERTY(Edit) LinearColor color = {1.0f, 1.0f, 1.0f, 1.0f};
};

LSD_STRUCT()
struct ParticleEmitter {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char name[32] = "Emitter";
    LSD_PROPERTY(Edit) bool enabled = true;
    // Spawn
    LSD_PROPERTY(Edit, Units = "/s", Min = 0, Tooltip = "Particles a second") f32 spawn_rate = 20.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 4096, Tooltip = "Particles all at once when it starts (and each loop)") u32 burst = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "How long it emits; 0 = forever") f32 duration = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Starts again after its duration") bool looping = true;
    LSD_PROPERTY(Edit, Min = 1, Max = 4096) u32 max_particles = 500;
    LSD_PROPERTY(Edit, Units = "s", Min = 0) f32 lifetime_min = 1.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0) f32 lifetime_max = 2.0f;
    // Shape and velocity (in the effect's space: rotate the object to aim it)
    LSD_PROPERTY(Edit, Widget = "Choice:Point,Sphere,Box") u32 shape = 0;
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Sphere: X is the radius. Box: half its size on each axis") Vec3 shape_size = {0.5f, 0.5f, 0.5f};
    LSD_PROPERTY(Edit, Tooltip = "Where they fly (normalised)") Vec3 direction = {0.0f, 0.0f, 1.0f};
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 180, Tooltip = "Cone around the direction: 0 = straight, 180 = every way") f32 spread = 15.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 speed_min = 1.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 speed_max = 2.0f;
    // Forces
    LSD_PROPERTY(Edit, Tooltip = "Times the World Settings gravity (0 = floats, negative = rises)") f32 gravity_scale = 0.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Tooltip = "Constant push (wind, buoyancy), world space") Vec3 acceleration = {};
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "Slows them down: fraction of speed lost a second") f32 drag = 0.0f;
    // Look
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "Empty: a soft round dot") char texture[33] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Alpha,Additive", Tooltip = "Additive: glows, adds light (fire, sparks, magic); Alpha: smoke, dust") u32 blend = 0;
    LSD_PROPERTY(Edit, Tooltip = "Colour and opacity at birth (values above 1 glow brighter when Additive)") LinearColor color_start = {1.0f, 1.0f, 1.0f, 1.0f};
    LSD_PROPERTY(Edit, Tooltip = "Colour and opacity at death (0 alpha fades out)") LinearColor color_end = {1.0f, 1.0f, 1.0f, 0.0f};
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 size_start = 0.2f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 size_end = 0.4f;
    LSD_PROPERTY(Edit, Units = "deg/s", Min = 0, Tooltip = "Sprites spin at a random speed up to this, either way") f32 spin = 0.0f;
    // Over life (Niagara's Scale Sprite Size / Scale Color): when set, these replace the start -> end blend.
    LSD_PROPERTY(Edit, Category = "Over Life", Tooltip = "Size over the particle's life, times Size Start (puff up then shrink...); empty = Start to End")
    List<LifeKey, 8> size_over_life;
    LSD_PROPERTY(Edit, Category = "Over Life", Tooltip = "Colour and opacity over life (fire: white, yellow, orange, smoke); empty = Start to End")
    List<LifeColorKey, 8> color_over_life;
    LSD_PROPERTY(Edit, Tooltip = "Particles move with the object (a torch carried around) instead of staying where they were born (a smoke trail)")
    bool local_space = false;
    // Renderer (Niagara's Sprite / Mesh Renderer).
    LSD_PROPERTY(Edit, Category = "Renderer", Widget = "Choice:Sprite,Mesh", Tooltip = "Sprite: camera-facing quads. Mesh: a lit mesh per particle (rocks, debris, shell casings)")
    u32 renderer = 0;
    LSD_PROPERTY(Edit, Category = "Renderer", Widget = "Asset:Mesh", Tooltip = "Mesh renderer: a mesh asset or a shape (Cube, Sphere); Size scales it") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Category = "Renderer", Tooltip = "Mesh renderer: points its +X along the way it flies (otherwise it turns by Spin)") bool align_to_velocity = false;
    // Collision (Niagara's Collision module); world-space emitters only.
    LSD_PROPERTY(Edit, Category = "Collision", Widget = "Choice:None,Floor,World", Tooltip = "Floor: a flat floor at Floor Height (also in the editor). World: what physics collides with (in Play)")
    u32 collision = 0;
    LSD_PROPERTY(Edit, Category = "Collision", Units = "m", Tooltip = "Floor: its height (world Z)") f32 floor_height = 0.0f;
    LSD_PROPERTY(Edit, Category = "Collision", Min = 0, Max = 1, Tooltip = "Speed kept off the surface (0 = stops dead, 1 = bounces fully)") f32 bounce = 0.4f;
    LSD_PROPERTY(Edit, Category = "Collision", Min = 0, Max = 1, Tooltip = "Speed lost along the surface on each hit") f32 friction = 0.2f;
    LSD_PROPERTY(Edit, Category = "Collision", Tooltip = "Dies on its first hit (rain drops, bullets)") bool kill_on_hit = false;
};

constexpr u32 kMaxEmitters = 8;
constexpr u32 kMaxParticles = 4096;  // per emitter

LSD_STRUCT(Category = "Effects")
struct ParticleSystem {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<ParticleEmitter, 8> emitters;
};
// shortcut: over-life keys are straight lines, no curve tangents; mesh and ribbon renderers next.

// A particle's size and colour at `t` (0..1 through its life): the over-life keys, else start -> end.
f32 size_at(const ParticleEmitter& e, f32 t);
LinearColor color_at(const ParticleEmitter& e, f32 t);

struct Particle {
    Vec3 position;  // local space if the emitter is, else world
    Vec3 velocity;
    f32 age, lifetime;
    f32 rotation, spin;  // degrees, degrees a second
};

// World collision for particles: the first surface on the segment from -> to (point and normal); false if none.
struct ParticleCollider {
    bool (*segment)(void* user, Vec3 from, Vec3 to, Vec3& point, Vec3& normal) = nullptr;
    void* user = nullptr;
    u32 budget = 1024;  // rays an update may cast across all particles (the rest move on unchecked this frame)
};

// One playing copy of a system (one per ParticleEffect object).
class ParticleInstance {
public:
    explicit ParticleInstance(Allocator& allocator) : allocator_(allocator) {}
    ~ParticleInstance();
    ParticleInstance(const ParticleInstance&) = delete;
    ParticleInstance& operator=(const ParticleInstance&) = delete;

    // Advances by dt. `world` is the effect's transform; `emitting` false lets the particles die out.
    // `collider`: what World collision tests against (none: World collision does nothing).
    void update(const ParticleSystem& system, const Mat4& world, f32 dt, f32 gravity, bool emitting, ParticleCollider* collider = nullptr);
    void restart();          // no particles, every emitter back to its start (and its burst)
    bool finished() const;   // nothing alive and nothing more to emit (every emitter non-looping and done)
    u32 count(u32 emitter) const { return emitter < kMaxEmitters ? emitters_[emitter].count : 0; }
    const Particle* particles(u32 emitter) const { return emitter < kMaxEmitters ? emitters_[emitter].particles : nullptr; }

    // Two triangles per particle facing the camera (`right` / `up`: the camera's axes in world space),
    // back to front for Alpha blending. Returns the vertex count written (6 per particle, up to capacity).
    u32 build_sprites(const ParticleSystem& system, u32 emitter, const Mat4& world, Vec3 eye, Vec3 right, Vec3 up,
                      rhi::ParticleVertex* out, u32 capacity) const;

private:
    struct Emitter {
        Particle* particles = nullptr;
        u32 count = 0, capacity = 0;
        f32 time = 0.0f;        // since the start of this loop
        f32 spawn_carry = 0.0f; // fraction of a particle owed by the rate
        bool burst_done = false;
        bool done = false;      // non-looping and past its duration
    };
    void spawn(const ParticleEmitter& e, Emitter& state, const Mat4& world);
    f32 random();  // 0..1

    Allocator& allocator_;
    Emitter emitters_[kMaxEmitters];
    u32 seed_ = 0x9E3779B9u;
};

} // namespace lsd::particles
