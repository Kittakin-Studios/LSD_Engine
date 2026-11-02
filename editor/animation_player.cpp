#include "animation_player.h"

#include "lsd/foundation/hash_id.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::editor {

namespace {

// Play Sound notifies of `clip` between `from` and `to` seconds (wrapped: went past the end and round).
void fire_notifies(const ClipSet& set, u32 clip, f32 from, f32 to, bool wrapped, Vec3 at, SoundPlayer& sounds) {
    for (const asset::AnimNotify& n : set.meta.notifies) {
        if (n.clip != clip || !n.sound[0]) continue;
        const bool crossed = wrapped ? (n.time > from || n.time <= to) : (n.time > from && n.time <= to);
        if (crossed) sounds.play(n.sound, true, at);
    }
}

} // namespace

AnimationPlayer::~AnimationPlayer() {
    for (State& s : states_) {
        if (s.morphed.valid()) device_.destroy_buffer(s.morphed);
    }
}

rhi::BufferHandle AnimationPlayer::vertices(world::Entity e, rhi::BufferHandle model) const {
    for (const State& s : states_) {
        if (s.used && s.entity == e) return s.morphed.valid() ? s.morphed : model;
    }
    return model;
}

// shortcut: morphs on the CPU, a new buffer per change (buffers are immutable); move to the GPU
// (a delta buffer read in the skinning shader) when many faces animate at once
void AnimationPlayer::update_morphs(State& state, const world::SkeletalMesh& mesh, const SkinnedEntry& model) {
    u64 key = 1469598103934665603ull ^ model.vertices.value;  // a reload makes a new buffer, so a new key
    bool any = false;
    for (const world::MorphWeight& m : mesh.morphs) {
        if (m.weight == 0.0f) continue;
        u32 bits;
        std::memcpy(&bits, &m.weight, sizeof bits);
        key = (key ^ hash64(m.name) ^ bits) * 1099511628211ull;
        any = true;
    }
    if (!any || model.model->morphs.empty()) key = 0;  // nothing to mix: the shared buffer
    if (key == state.morph_key) return;
    state.morph_key = key;
    if (state.morphed.valid()) device_.destroy_buffer(state.morphed);
    state.morphed = {};
    if (key == 0) return;
    std::vector<rhi::SkinnedVertex> morphed(model.model->vertices.size());
    anim::apply_morphs(*model.model, [&](const char* name) {
        f32 w = 0.0f;
        for (const world::MorphWeight& m : mesh.morphs) w = std::strcmp(m.name, name) == 0 ? m.weight : w;
        return w;
    }, morphed.data());
    state.morphed = device_.create_buffer(rhi::BufferUsage::Vertex, morphed.data(), morphed.size() * sizeof(rhi::SkinnedVertex));
}

const anim::Pose* AnimationPlayer::pose(world::Entity e) const {
    for (const State& s : states_) {
        if (s.used && s.entity == e) return &s.pose;
    }
    return nullptr;
}

void AnimationPlayer::reset() {
    for (State& s : states_) {
        s.time = 0.0f;
        s.graph = anim::GraphState{};
        s.notify_clip = ~0u;
    }
}

void AnimationPlayer::set_param(const world::World& world, world::Entity e, const char* name, f32 value) {
    for (State& s : states_) {
        if (!s.used) continue;
        for (world::Entity p = s.entity; p.valid(); p = world.parent(p)) {
            if (p == e) {
                s.graph.set(name, value);
                break;
            }
        }
    }
}

void AnimationPlayer::add_ik(world::Entity e, const char* bone, Vec3 target, f32 weight, bool aim, Vec3 forward) {
    if (ik_count_ == sizeof iks_ / sizeof iks_[0]) return;
    Ik& ik = iks_[ik_count_++];
    ik.entity = e;
    std::snprintf(ik.bone, sizeof ik.bone, "%s", bone);
    ik.target = target;
    ik.forward = forward;
    ik.weight = weight;
    ik.aim = aim;
}

// After sampling: Foot IK, then the scripts' requests, all in the mesh's model space.
void AnimationPlayer::apply_ik(world::World& world, State& state, const anim::SkinnedModel& model) {
    const Mat4 to_world = world.world_matrix(state.entity);
    const Mat4 to_model = inverse(to_world);
    if (const auto* feet = probe_ ? world.get<world::FootIK>(state.entity) : nullptr) {
        for (const char* bone : {feet->left_foot, feet->right_foot}) {
            const i32 j = model.find_joint(bone);
            if (j < 0) continue;
            const Mat4 foot = to_world * state.pose.joint_model(static_cast<u32>(j));
            const Vec3 at{foot.at(0, 3), foot.at(1, 3), foot.at(2, 3)};
            Vec3 ground;
            if (!probe_(probe_user_, at + Vec3{0, 0, feet->max_step}, feet->max_step * 2.0f, ground)) continue;
            const f32 want = ground.z + feet->foot_height;
            if (want <= at.z + 0.01f) continue;  // shortcut: feet only go up onto steps; lowering needs a pelvis offset
            state.pose.two_bone_ik(model, static_cast<u32>(j), transform_point(to_model, Vec3{at.x, at.y, want}), 1.0f);
        }
    }
    for (u32 i = 0; i < ik_count_; ++i) {
        const Ik& ik = iks_[i];
        if (ik.entity != state.entity) continue;
        const i32 j = model.find_joint(ik.bone);
        if (j < 0) continue;
        const Vec3 target = transform_point(to_model, ik.target);
        if (ik.aim) state.pose.aim_ik(model, static_cast<u32>(j), target, ik.forward, ik.weight);
        else state.pose.two_bone_ik(model, static_cast<u32>(j), target, ik.weight);
    }
}

bool AnimationPlayer::socket(const world::World& world, const SkinnedCache& cache, world::Entity e, const char* name, Mat4& out) {
    const auto* mesh = world.alive(e) ? world.get<world::SkeletalMesh>(e) : nullptr;
    const anim::Pose* p = mesh ? pose(e) : nullptr;
    asset::Guid guid;
    const asset::AssetRecord* r = p && asset::parse_guid(mesh->mesh, guid) ? cache.registry().find(guid) : nullptr;
    if (!r) return false;
    Sockets* found = nullptr;
    for (Sockets& s : sockets_) {
        if (std::strcmp(s.guid, mesh->mesh) == 0) found = &s;
    }
    if (!found) {
        sockets_.push_back({});
        found = &sockets_.back();
        std::snprintf(found->guid, sizeof found->guid, "%s", mesh->mesh);
        found->modified = -1;
    }
    char meta_path[300];
    cache.registry().source_path(*r, meta_path, sizeof meta_path);
    std::error_code error;
    const i64 modified = static_cast<i64>(std::filesystem::last_write_time(std::string(meta_path) + ".meta", error).time_since_epoch().count());
    if (modified != found->modified) {
        found->meta = asset::AssetMeta{};
        cache.registry().load_meta(*r, found->meta);
        found->modified = modified;
    }
    for (const asset::MeshSocket& s : found->meta.sockets) {
        if (std::strcmp(s.name, name) != 0) continue;
        const SkinnedEntry* model = const_cast<SkinnedCache&>(cache).get(mesh->mesh);
        const i32 j = model && model->model ? model->model->find_joint(s.bone) : -1;
        if (j < 0) return false;
        out = world.world_matrix(e) * p->joint_model(static_cast<u32>(j)) * trs(s.position, s.rotation, Vec3{1, 1, 1});
        return true;
    }
    return false;
}

void AnimationPlayer::attach_to_sockets(world::World& world, const SkinnedCache& cache) {
    const reflect::TypeInfo& type = reflect::type_of<world::AttachToSocket>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const auto* a = static_cast<const world::AttachToSocket*>(world.at(type, i));
        world::Entity target;
        for (u32 slot = 0; slot < world.slot_count() && !target.valid(); ++slot) {
            const world::Entity e = world.entity_at_slot(slot);
            if (e.valid() && std::strcmp(world.name(e), a->target) == 0) target = e;
        }
        Mat4 m;
        if (!target.valid() || !socket(world, cache, target, a->socket, m)) continue;
        const world::Entity self = world.owner(type, i);
        Vec3 t, r, scale;
        decompose_trs(world.world_matrix(self), t, r, scale);  // keeps its own size
        world.set_world_matrix(self, m * scaling(scale));
    }
}

void AnimationPlayer::update(world::World& world, SkinnedCache& cache, f32 dt, SoundPlayer* sounds) {
    for (State& s : states_) {  // objects deleted or without the component any more
        if (!s.used || (world.alive(s.entity) && world.get<world::SkeletalMesh>(s.entity))) continue;
        s.used = false;
        if (s.morphed.valid()) device_.destroy_buffer(s.morphed);
        s.morphed = {};
        s.morph_key = 0;
    }
    const reflect::TypeInfo& type = reflect::type_of<world::SkeletalMesh>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const world::Entity e = world.owner(type, i);
        const auto* mesh = static_cast<const world::SkeletalMesh*>(world.at(type, i));
        const SkinnedEntry* model = cache.get(mesh->mesh);
        if (!model) continue;
        State* state = nullptr;
        for (State& s : states_) {
            if (s.used && s.entity == e) state = &s;
        }
        for (State& s : states_) {
            if (!state && !s.used) {
                state = &s;
                s.used = true;
                s.entity = e;
                s.time = 0.0f;
                s.graph = anim::GraphState{};
                s.notify_clip = ~0u;
            }
        }
        if (!state) continue;  // more than 256 animated objects: the rest stay still
        update_morphs(*state, *mesh, *model);
        const Mat4 m = world.world_matrix(e);
        const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};

        // Animation Graph: states, blend spaces and crossfades pick the clips.
        if (const anim::AnimGraph* graph = mesh->anim_graph[0] ? cache.graph(mesh->anim_graph) : nullptr) {
            const ClipSet* set = cache.clips(mesh->mesh, graph->animation[0] ? graph->animation : mesh->animation);
            anim::BlendInput inputs[anim::kMaxBlendInputs];
            const u32 n = anim::step_graph(*graph, set ? set->clips : nullptr, set ? set->count : 0, state->graph, mesh->playing ? dt * mesh->play_rate : 0.0f, inputs);
            state->pose.sample_blend(*model->model, inputs, n);
            apply_ik(world, *state, *model->model);
            // Notifies follow the strongest clip (a blend space's feet are in step, so one is enough).
            u32 best = 0;
            for (u32 k = 1; k < n; ++k) {
                if (inputs[k].weight > inputs[best].weight) best = k;
            }
            const u32 clip = n && set && inputs[best].clip ? static_cast<u32>(inputs[best].clip - set->clips) : ~0u;
            const f32 ratio = n ? inputs[best].ratio : 0.0f;
            if (sounds && clip != ~0u && clip == state->notify_clip && ratio != state->notify_ratio) {
                const f32 duration = set->clips[clip].duration;
                fire_notifies(*set, clip, state->notify_ratio * duration, ratio * duration, ratio < state->notify_ratio, at, *sounds);
            }
            state->notify_clip = clip;
            state->notify_ratio = ratio;
            continue;
        }

        const ClipSet* set = cache.clips(mesh->mesh, mesh->animation);
        const anim::Clip* clip = set && mesh->clip < set->count ? &set->clips[mesh->clip] : nullptr;
        const f32 before = state->time;
        if (clip && mesh->playing) state->time += dt * mesh->play_rate;
        state->pose.sample(*model->model, clip, state->time, mesh->looping);
        apply_ik(world, *state, *model->model);

        // Notifies whose time this frame's advance crossed (wrapping when looping).
        if (!sounds || !clip || clip->duration <= 0.0f || state->time == before) continue;
        const f32 duration = clip->duration;
        auto local = [&](f32 t) { return mesh->looping ? t - std::floor(t / duration) * duration : (t < 0.0f ? 0.0f : t > duration ? duration : t); };
        const f32 from = local(before), to = local(state->time);
        fire_notifies(*set, mesh->clip, from, to, mesh->looping && to < from, at, *sounds);
    }
    ik_count_ = 0;  // requests last one frame
}

} // namespace lsd::editor
