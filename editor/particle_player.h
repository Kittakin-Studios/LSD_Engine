#pragma once
// Plays the ParticleEffect components: loads their Particle System assets (reloaded when the file
// changes), simulates one instance per object, and draws them as sprites after the opaque meshes.
// Effects preview in the editor; Play restarts them.
#include "material_cache.h"

#include "lsd/particles/particles.h"
#include "lsd/world/world.h"

namespace lsd::editor {

class ParticlePlayer {
public:
    ParticlePlayer(rhi::Device& device, Allocator& allocator, const asset::AssetRegistry& registry, TextureCache& textures);
    ~ParticlePlayer();
    ParticlePlayer(const ParticlePlayer&) = delete;
    ParticlePlayer& operator=(const ParticlePlayer&) = delete;

    // `playing`: in Play (effects with Preview off only run then). dt 0 freezes them (pause).
    void update(const world::World& world, f32 dt, bool playing);
    // One viewport: `view` gives the camera's axes, so sprites face it.
    void draw(rhi::Device& device, const world::World& world, const Mat4& view_proj, const Mat4& view, Vec3 eye, bool skip_hidden);
    // One instance (the Particle Editor's preview), the same way.
    void draw_instance(rhi::Device& device, const particles::ParticleInstance& instance, const particles::ParticleSystem& system, const Mat4& world,
                       const Mat4& view_proj, const Mat4& view, Vec3 eye);
    const particles::ParticleSystem* system(const char* guid);  // loaded (and hot-reloaded) asset; nullptr if missing
    void reset();         // every effect starts over (Play)
    void check_files();   // about once a second
    u32 particle_count() const { return particle_count_; }

private:
    struct System {
        char guid[33];
        particles::ParticleSystem system;
        i64 modified = 0;
        bool failed = false;
    };
    struct Effect {
        world::Entity entity;
        particles::ParticleInstance* instance = nullptr;
        const System* system = nullptr;
        bool used = false;
    };
    const System* find(const char* guid);
    void load(System& s);

    Allocator& allocator_;
    const asset::AssetRegistry& registry_;
    TextureCache& textures_;
    rhi::TextureHandle dot_;  // default sprite: a soft round dot
    System* systems_[64] = {};
    u32 system_count_ = 0;
    Effect effects_[512];
    rhi::ParticleVertex* vertices_ = nullptr;  // one emitter's sprites at a time
    u32 particle_count_ = 0;
};

} // namespace lsd::editor
