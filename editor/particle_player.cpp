#include "particle_player.h"

#include "lighting.h"
#include "mesh_cache.h"

#include "lsd/asset/data_asset.h"

#include <vector>
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
    : allocator_(allocator), registry_(registry), textures_(textures), device_(device) {
    particles::set_gpu_particles(device.supports_gpu_particles());
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
    for (const GpuState& g : gpu_) {
        if (g.buffer.valid()) device_.destroy_buffer(g.buffer);
    }
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
    shadow_rays_ = 0;         // lit particles' sun rays, likewise
    playing_ = playing;
    for (Effect& e : effects_) {  // objects deleted or without the component any more
        if (e.used && (!world.alive(e.entity) || !world.get<world::ParticleEffect>(e.entity))) {
            e.used = false;
            e.instance->restart();
        }
    }
    particle_count_ = 0;
    finished_count_ = 0;
    ++frame_;
    for (usize i = 0; i < gpu_.size();) {  // GPU emitters not drawn for a few seconds (effect gone, emitter back on the CPU)
        if (frame_ - gpu_[i].frame < 300) {
            ++i;
            continue;
        }
        if (gpu_[i].buffer.valid()) device_.destroy_buffer(gpu_[i].buffer);
        gpu_[i] = gpu_.back();
        gpu_.pop_back();
    }
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

// The mesh shader's exponential height fog per vertex (the particles are already in world space): alpha
// particles take on the fog's colour, additive ones fade to nothing (fog adds no light of its own).
void ParticlePlayer::apply_fog(rhi::ParticleVertex* vertices, u32 count, Vec3 eye, bool additive) const {
    if (fog_[3] <= 0.0f) return;
    for (u32 i = 0; i < count; ++i) {
        rhi::ParticleVertex& v = vertices[i];
        const f32 f = particles::fog_amount(fog_, fog_color_[3], eye, {v.position[0], v.position[1], v.position[2]});
        for (int c = 0; c < 3; ++c) v.color[c] = additive ? v.color[c] * (1.0f - f) : v.color[c] + (fog_color_[c] - v.color[c]) * f;
    }
}

void ParticlePlayer::apply_lighting(rhi::ParticleVertex* vertices, u32 count, Vec3 eye) const {
    if (!lights_) return;
    // In Play, the sun's shadow: a ray toward the sun from each sprite (every 6 vertices: one quad), against what physics
    // collides with, as World collision does. shortcut: a sharp yes / no per sprite, no soft edge; at most 2048 a frame
    const i32 sun = static_cast<i32>(lights_->shadow_info[1]);
    const bool shadows = playing_ && collider_.segment && sun >= 0 && static_cast<u32>(sun) < lights_->count;
    f32 visible = 1.0f;
    for (u32 i = 0; i < count; ++i) {
        rhi::ParticleVertex& v = vertices[i];
        const Vec3 p{v.position[0], v.position[1], v.position[2]};
        if (shadows && i % 6 == 0 && shadow_rays_ < 2048) {
            ++shadow_rays_;
            const rhi::GpuLight& L = lights_->lights[sun];
            const Vec3 to_sun{-L.direction[0], -L.direction[1], -L.direction[2]};
            Vec3 hit, n;
            visible = collider_.segment(collider_.user, p + to_sun * 0.05f, p + to_sun * 60.0f, hit, n) ? 0.0f : 1.0f;
        }
        const Vec3 to_eye = eye - p;
        const f32 d = length(to_eye);
        const Vec3 light = particle_light(*lights_, p, d > 1e-4f ? to_eye * (1.0f / d) : Vec3{0.0f, 0.0f, 1.0f}, shadows ? visible : 1.0f);
        v.color[0] *= light.x;
        v.color[1] *= light.y;
        v.color[2] *= light.z;
    }
}

void ParticlePlayer::draw_instance(rhi::Device& device, const particles::ParticleInstance& instance, const particles::ParticleSystem& system,
                                   const Mat4& world, const Mat4& view_proj, const Mat4& view, Vec3 eye) {
    const Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};  // the view matrix's rows: camera axes in world space
    const Mat4 projection = view_proj * inverse(view);  // soft particles turn depth back into distance with it
    const Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
    const f32 depth_terms[4] = {0.0f, projection.at(2, 2), projection.at(2, 3), projection.at(3, 3) > 0.5f ? 1.0f : 0.0f};  // soft particles
    for (u32 k = 0; k < system.emitters.count; ++k) {
        const particles::ParticleEmitter emitter = particles::active_modules(system.emitters[k]);
        if (particles::GpuStep step; instance.gpu_step(k, step)) {  // simulated and drawn on the GPU
            draw_gpu(device, instance, k, emitter, world, view_proj, right, up, depth_terms);
            continue;
        }
        if (emitter.renderer == 1) {  // a lit mesh per particle, in one instanced draw
            const CachedMesh* mesh = meshes_ ? meshes_->get(emitter.mesh) : nullptr;
            const u32 count = instance.count(k);
            if (!mesh || !count) continue;
            static std::vector<f32> rows;
            rows.clear();
            const particles::Particle* ps = instance.particles(k);
            for (u32 i = 0; i < count; ++i) {
                const particles::Particle& p = ps[i];
                const f32 t = p.lifetime > 0.0f ? p.age / p.lifetime : 0.0f;
                const f32 size = particles::size_at(emitter, t);
                Vec3 position = emitter.local_space ? transform_point(world, p.position) : p.position;
                Vec3 rotation{0.0f, 0.0f, p.rotation};
                if (emitter.align_to_velocity && dot(p.velocity, p.velocity) > 1e-6f) {
                    const Vec3 v = emitter.local_space ? transform_vector(world, p.velocity) : p.velocity;
                    rotation = {0.0f, -degrees(std::atan2(v.z, std::sqrt(v.x * v.x + v.y * v.y))), degrees(std::atan2(v.y, v.x))};
                }
                const Mat4 m = trs(position, rotation, {size, size, size});
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) rows.push_back(m.at(r, c));
                }
            }
            rhi::MeshDraw draw{};
            draw.vertices = mesh->gpu.vertices;
            draw.indices = mesh->gpu.indices;
            draw.index_count = mesh->gpu.index_count;
            std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
            draw.camera_position[0] = eye.x;
            draw.camera_position[1] = eye.y;
            draw.camera_position[2] = eye.z;
            // shortcut: one colour for every copy (instances carry no colour yet): the start colour.
            draw.color[0] = emitter.color_start.r;
            draw.color[1] = emitter.color_start.g;
            draw.color[2] = emitter.color_start.b;
            draw.color[3] = 1.0f;
            draw.roughness = 0.6f;
            if (emitter.texture[0]) draw.base_color_texture = textures_.get(emitter.texture, true);
            draw.instance_rows = rows.data();
            draw.instance_count = count;
            device.draw_mesh(draw);
            continue;
        }
        const u32 n = emitter.renderer == 2 ? instance.build_ribbon(system, k, world, eye, vertices_, kMaxVertices)
                                             : instance.build_sprites(system, k, world, eye, right, up, vertices_, kMaxVertices);
        if (!n) continue;
        if (emitter.lit && emitter.blend == 0) apply_lighting(vertices_, n, eye);  // before the fog, as the meshes
        apply_fog(vertices_, n, eye, emitter.blend == 1);
        const rhi::TextureHandle texture = emitter.texture[0] ? textures_.get(emitter.texture, true) : rhi::TextureHandle{};
        f32 soft[4] = {emitter.soft_distance, projection.at(2, 2), projection.at(2, 3), projection.at(3, 3) > 0.5f ? 1.0f : 0.0f};
        device.draw_particles(vertices_, n, view_proj.m, texture.valid() ? texture : dot_, emitter.blend == 1, emitter.soft_distance > 0.0f ? soft : nullptr);
    }
}

// A GPU emitter: its buffer (made again when Max Particles changes or the instance restarts), one simulation per
// instance step, then its sprites. shortcut: a second instance update before a draw loses the first step's new
// particles; carry the counts over if effects ever update more often than they draw.
void ParticlePlayer::draw_gpu(rhi::Device& device, const particles::ParticleInstance& instance, u32 k, const particles::ParticleEmitter& emitter,
                              const Mat4& world, const Mat4& view_proj, Vec3 right, Vec3 up, const f32 soft_in[4]) {
    particles::GpuStep step;
    if (!instance.gpu_step(k, step)) return;
    GpuState* state = nullptr;
    for (GpuState& g : gpu_) {
        if (g.instance == &instance && g.emitter == k) state = &g;
    }
    if (!state) state = &gpu_.emplace_back(GpuState{&instance, k, 0, 0, 0, 0, {}});
    if (state->buffer.valid() && (state->capacity != step.capacity || state->restarts != step.restarts)) {
        device.destroy_buffer(state->buffer);
        state->buffer = {};
    }
    if (!state->buffer.valid()) {
        state->buffer = device.create_particle_state(step.capacity);
        state->capacity = step.capacity;
        state->restarts = step.restarts;
        state->steps = step.steps - 1;  // simulate this step
        if (!state->buffer.valid()) return;
    }
    state->frame = frame_;
    rhi::GpuParticles params;
    const u32 seed = step.steps * 2654435761u ^ (k + 1) * 40503u ^ static_cast<u32>(reinterpret_cast<uintptr_t>(&instance) >> 4);
    particles::gpu_params(emitter, step, world, seed, params);
    if (state->steps != step.steps) {
        device.simulate_particles(state->buffer, params);
        state->steps = step.steps;
    }
    std::memcpy(params.right, &right, sizeof(Vec3));
    std::memcpy(params.up, &up, sizeof(Vec3));
    const rhi::TextureHandle texture = emitter.texture[0] ? textures_.get(emitter.texture, true) : rhi::TextureHandle{};
    f32 soft[4];
    std::memcpy(soft, soft_in, sizeof soft);
    soft[0] = emitter.soft_distance;
    device.draw_gpu_particles(state->buffer, params, view_proj.m, texture.valid() ? texture : dot_, emitter.blend == 1, emitter.soft_distance > 0.0f ? soft : nullptr);
}

void ParticlePlayer::draw(rhi::Device& device, const world::World& world, const Mat4& view_proj, const Mat4& view, Vec3 eye, bool skip_hidden) {
    for (const Effect& e : effects_) {
        if (!e.used || !e.system || !world.alive(e.entity)) continue;
        if (skip_hidden && world.hidden_in_hierarchy(e.entity)) continue;
        draw_instance(device, *e.instance, e.system->system, world.world_matrix(e.entity), view_proj, view, eye);
    }
}

} // namespace lsd::editor
