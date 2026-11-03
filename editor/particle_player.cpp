#include "particle_player.h"

#include "lsd/asset/data_asset.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

namespace lsd::editor {

namespace {

constexpr u32 kMaxVertices = particles::kMaxParticles * 6;

i64 file_time(const char* path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

bool path_of(const asset::AssetRegistry& registry, const char* guid_text, char* out, usize capacity) {
    asset::Guid guid;
    const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (!r) return false;
    registry.source_path(*r, out, capacity);
    return true;
}

} // namespace

ParticlePlayer::ParticlePlayer(rhi::Device& device, Allocator& allocator, const asset::AssetRegistry& registry, TextureCache& textures)
    : allocator_(allocator), registry_(registry), textures_(textures) {
    // A 64x64 soft dot: white, alpha falling smoothly to 0 at the edge.
    static u8 pixels[64 * 64 * 4];
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f) / 32.0f - 1.0f, dy = (static_cast<f32>(y) + 0.5f) / 32.0f - 1.0f;
            const f32 r = std::sqrt(dx * dx + dy * dy);
            const f32 a = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r) * (1.0f + 2.0f * r);  // smoothstep falloff
            u8* p = pixels + (y * 64 + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
    const void* mips[1] = {pixels};
    dot_ = device.create_texture(64, 64, 1, rhi::TextureFormat::RGBA8, mips);
    vertices_ = static_cast<rhi::ParticleVertex*>(allocator_.allocate(sizeof(rhi::ParticleVertex) * kMaxVertices, alignof(rhi::ParticleVertex)));
}

ParticlePlayer::~ParticlePlayer() {
    for (Effect& e : effects_) {
        if (e.instance) {
            e.instance->~ParticleInstance();
            allocator_.deallocate(e.instance);
        }
    }
    for (u32 i = 0; i < system_count_; ++i) delete systems_[i];
    allocator_.deallocate(vertices_);
}

void ParticlePlayer::load(System& s) {
    s.system = particles::ParticleSystem{};
    char path[300];
    s.failed = !path_of(registry_, s.guid, path, sizeof path) || !asset::load_data_asset(path, s.system);
    s.modified = s.failed ? 0 : file_time(path);
}

const ParticlePlayer::System* ParticlePlayer::find(const char* guid) {
    if (!guid[0]) return nullptr;
    for (u32 i = 0; i < system_count_; ++i) {
        if (std::strcmp(systems_[i]->guid, guid) == 0) return systems_[i]->failed ? nullptr : systems_[i];
    }
    if (system_count_ == sizeof systems_ / sizeof systems_[0]) return nullptr;
    System* s = systems_[system_count_++] = new System();
    std::snprintf(s->guid, sizeof s->guid, "%s", guid);
    load(*s);
    return s->failed ? nullptr : s;
}

const particles::ParticleSystem* ParticlePlayer::system(const char* guid) {
    const System* s = find(guid);
    return s ? &s->system : nullptr;
}

void ParticlePlayer::check_files() {
    for (u32 i = 0; i < system_count_; ++i) {
        char path[300];
        if (path_of(registry_, systems_[i]->guid, path, sizeof path) && file_time(path) != systems_[i]->modified) load(*systems_[i]);
    }
}

void ParticlePlayer::reset() {
    for (Effect& e : effects_) {
        if (e.instance) e.instance->restart();
    }
}

void ParticlePlayer::update(const world::World& world, f32 dt, bool playing) {
    collider_.budget = 1024;  // World collision rays this frame, for every effect together
    for (Effect& e : effects_) {  // objects deleted or without the component any more
        if (e.used && (!world.alive(e.entity) || !world.get<world::ParticleEffect>(e.entity))) {
            e.used = false;
            e.instance->restart();
        }
    }
    particle_count_ = 0;
    finished_count_ = 0;
    const reflect::TypeInfo& type = reflect::type_of<world::ParticleEffect>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const world::Entity entity = world.owner(type, i);
        const auto* c = static_cast<const world::ParticleEffect*>(world.at(type, i));
        const System* system = find(c->system);
        Effect* effect = nullptr;
        for (Effect& e : effects_) {
            if (e.used && e.entity == entity) effect = &e;
        }
        for (Effect& e : effects_) {
            if (!effect && !e.used) {
                effect = &e;
                e.used = true;
                e.entity = entity;
                if (!e.instance) e.instance = new (allocator_.allocate(sizeof(particles::ParticleInstance), alignof(particles::ParticleInstance)))
                    particles::ParticleInstance(allocator_);
            }
        }
        if (!effect) continue;  // more than 512 effects: the rest stay still
        effect->system = system;
        if (!system || (!playing && !c->preview)) {
            effect->instance->restart();
            continue;
        }
        effect->instance->update(system->system, world.world_matrix(entity), dt * c->time_scale, world.settings().gravity, c->active,
                                 playing && collider_.segment ? &collider_ : nullptr);
        for (u32 k = 0; k < particles::kMaxEmitters; ++k) particle_count_ += effect->instance->count(k);
        if (playing && c->auto_destroy && effect->instance->finished() && finished_count_ < sizeof finished_ / sizeof finished_[0]) {
            finished_[finished_count_++] = entity;
        }
    }
}

u32 ParticlePlayer::take_finished(world::Entity* out, u32 capacity) {
    const u32 n = finished_count_ < capacity ? finished_count_ : capacity;
    for (u32 i = 0; i < n; ++i) out[i] = finished_[i];
    finished_count_ = 0;
    return n;
}

void ParticlePlayer::draw_instance(rhi::Device& device, const particles::ParticleInstance& instance, const particles::ParticleSystem& system,
                                   const Mat4& world, const Mat4& view_proj, const Mat4& view, Vec3 eye) {
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};  // the view matrix's rows: camera axes in world space
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    for (u32 k = 0; k < system.emitters.count; ++k) {
        const u32 n = instance.build_sprites(system, k, world, eye, right, up, vertices_, kMaxVertices);
        if (!n) continue;
        const particles::ParticleEmitter& emitter = system.emitters[k];
        const rhi::TextureHandle texture = emitter.texture[0] ? textures_.get(emitter.texture, true) : rhi::TextureHandle{};
        device.draw_particles(vertices_, n, view_proj.m, texture.valid() ? texture : dot_, emitter.blend == 1);
    }
}

void ParticlePlayer::draw(rhi::Device& device, const world::World& world, const Mat4& view_proj, const Mat4& view, Vec3 eye, bool skip_hidden) {
    for (const Effect& e : effects_) {
        if (!e.used || !e.system || !world.alive(e.entity)) continue;
        if (skip_hidden && world.hidden_in_hierarchy(e.entity)) continue;
        draw_instance(device, *e.instance, e.system->system, world.world_matrix(e.entity), view_proj, view, eye);
    }
}

} // namespace lsd::editor
