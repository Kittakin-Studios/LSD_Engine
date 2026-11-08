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
    void set_meshes(class MeshCache* meshes) { meshes_ = meshes; }  // for the Mesh renderer
    // What particles with World collision hit in Play (null: nothing to hit, Floor still works).
    void set_collider(bool (*segment)(void* user, Vec3 from, Vec3 to, Vec3& point, Vec3& normal), void* user) {
        collider_.segment = segment;
        collider_.user = user;
    }
    // In Play: objects whose Auto Destroy effect finished during the last update (the caller deletes them).
    u32 take_finished(world::Entity* out, u32 capacity);
    // One viewport: `view` gives the camera's axes, so sprites face it.
    void draw(rhi::Device& device, const world::World& world, const Mat4& view_proj, const Mat4& view, Vec3 eye, bool skip_hidden);
    // One instance (the Particle Editor's preview), the same way.
    void draw_instance(rhi::Device& device, const particles::ParticleInstance& instance, const particles::ParticleSystem& system, const Mat4& world,
                       const Mat4& view_proj, const Mat4& view, Vec3 eye);
    const particles::ParticleSystem* system(const char* guid);  // loaded (and hot-reloaded) asset; nullptr if missing
    void reset();         // every effect starts over (Play)
    void check_files();   // about once a second
    u32 particle_count() const { return particle_count_; }
    // The view's lights for Lit emitters (kept by the caller, updated per view; null: drawn unlit).
    void set_lights(const rhi::LightSetup* lights) { lights_ = lights; }
    // The view's height fog (rhi::LightSetup fog / fog_color): sprites and ribbons fade into it like meshes.
    void set_fog(const f32 fog[4], const f32 fog_color[4]) {
        for (int k = 0; k < 4; ++k) fog_[k] = fog[k], fog_color_[k] = fog_color[k];
    }

private:
    f32 fog_[4] = {}, fog_color_[4] = {};
    void apply_fog(rhi::ParticleVertex* vertices, u32 count, Vec3 eye, bool additive) const;
    const rhi::LightSetup* lights_ = nullptr;
    void apply_lighting(rhi::ParticleVertex* vertices, u32 count, Vec3 eye) const;
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
    world::Entity finished_[64];
    u32 finished_count_ = 0;
    u32 particle_count_ = 0;
    particles::ParticleCollider collider_;
    class MeshCache* meshes_ = nullptr;
};

} // namespace lsd::editor
