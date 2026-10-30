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
    LSD_PROPERTY(Edit, Tooltip = "Particles move with the object (a torch carried around) instead of staying where they were born (a smoke trail)")
    bool local_space = false;
};

constexpr u32 kMaxEmitters = 8;
constexpr u32 kMaxParticles = 4096;  // per emitter

LSD_STRUCT(Category = "Effects")
struct ParticleSystem {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) List<ParticleEmitter, 8> emitters;
};
// shortcut: start -> end colour / size only; add Curve assets over life, collision, mesh and ribbon renderers next.

struct Particle {
    Vec3 position;  // local space if the emitter is, else world
    Vec3 velocity;
    f32 age, lifetime;
    f32 rotation, spin;  // degrees, degrees a second
};

// One playing copy of a system (one per ParticleEffect object).
class ParticleInstance {
public:
    explicit ParticleInstance(Allocator& allocator) : allocator_(allocator) {}
    ~ParticleInstance();
    ParticleInstance(const ParticleInstance&) = delete;
    ParticleInstance& operator=(const ParticleInstance&) = delete;

    // Advances by dt. `world` is the effect's transform; `emitting` false lets the particles die out.
    void update(const ParticleSystem& system, const Mat4& world, f32 dt, f32 gravity, bool emitting);
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
