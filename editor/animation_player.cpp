#include "animation_player.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/log.h"

#include <cctype>
#include <cmath>
#include <initializer_list>
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
    // A Morphs row on the component wins (scripts, hand-set); otherwise the playing clips' curves.
    auto weight_of = [&](const char* name) {
        for (const world::MorphWeight& m : mesh.morphs) {
            if (std::strcmp(m.name, name) == 0) return m.weight;
        }
        f32 w = 0.0f;
        anim::blended_morph_weight(state.inputs, state.input_count, name, w);
        return w;
    };
    u64 key = 1469598103934665603ull ^ model.vertices.value;  // a reload makes a new buffer, so a new key
    bool any = false;
    for (const anim::MorphTarget& t : model.model->morphs) {  // shortcut: names shared by several meshes hash twice; harmless
        const f32 w = weight_of(t.name);
        if (w == 0.0f) continue;
        u32 bits;
        std::memcpy(&bits, &w, sizeof bits);
        key = (key ^ hash64(t.name) ^ bits) * 1099511628211ull;
        any = true;
    }
    if (!any) key = 0;  // nothing to mix: the shared buffer
    if (key == state.morph_key) return;
    state.morph_key = key;
    if (state.morphed.valid()) device_.destroy_buffer(state.morphed);
    state.morphed = {};
    if (key == 0) return;
    std::vector<rhi::SkinnedVertex> morphed(model.model->vertices.size());
    anim::apply_morphs(*model.model, weight_of, morphed.data());
    state.morphed = device_.create_buffer(rhi::BufferUsage::Vertex, morphed.data(), morphed.size() * sizeof(rhi::SkinnedVertex));
}

const char* AnimationPlayer::graph_state(SkinnedCache& cache, const world::SkeletalMesh& mesh, world::Entity e, const char** fading_from) const {
    if (fading_from) *fading_from = "";
    const anim::AnimGraph* graph = mesh.anim_graph[0] ? cache.graph(mesh.anim_graph) : nullptr;
    for (const State& s : states_) {
        if (!graph || !s.used || s.entity != e) continue;
        if (fading_from && s.graph.previous >= 0 && s.graph.previous < static_cast<i32>(graph->states.count)) *fading_from = graph->states[static_cast<u32>(s.graph.previous)].name;
        return anim::state_name(*graph, s.graph);
    }
    return "";
}

void AnimationPlayer::set_motion_input(world::Entity character, const anim::MotionInput& input) {
    for (MotionInputs& m : motion_inputs_) {
        if (m.character == character) {
            m.input = input;
            return;
        }
    }
    motion_inputs_.push_back({character, input});
}

bool AnimationPlayer::motion_readout(world::Entity e, char* out, usize capacity) const {
    for (const State& s : states_) {
        if (!s.used || s.entity != e || !s.motion_data || s.motion.clip < 0) continue;
        std::snprintf(out, capacity, "%s at %.2f s (match %.2f)%s", s.motion_data->names[s.motion.clip], static_cast<f64>(s.motion.time), static_cast<f64>(s.motion.cost),
                      s.motion.fade < 1.0f && s.motion.previous_clip >= 0 ? ", blending" : "");
        return true;
    }
    return false;
}

namespace {

bool contains_word(const char* name, std::initializer_list<const char*> words) {
    char lowered[96];
    usize i = 0;
    for (; name[i] && i + 1 < sizeof lowered; ++i) lowered[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    lowered[i] = '\0';
    for (const char* w : words) {
        if (std::strstr(lowered, w)) return true;
    }
    return false;
}

} // namespace

// The Motion Database `character` asks for, built on `mesh`'s skeleton: its listed clips, or (none
// listed) every locomotion clip in the mesh's folder (Idle, Walk, Run, Jog, Sprint, Strafe, Turn,
// Start, Stop, Pivot in the file or take name; one-shots are those that start, stop, turn or pivot).
const AnimationPlayer::Motion* AnimationPlayer::motion_for(SkinnedCache& cache, const world::SkeletalMesh& mesh, const SkinnedEntry& model, const world::Character& character) {
    Motion* m = nullptr;
    for (auto& candidate : motions_) {
        if (std::strcmp(candidate->mesh, mesh.mesh) == 0 && std::strcmp(candidate->database, character.motion_database) == 0) m = candidate.get();
    }
    if (!m) {
        motions_.push_back(std::make_unique<Motion>());
        m = motions_.back().get();
        std::snprintf(m->mesh, sizeof m->mesh, "%s", mesh.mesh);
        std::snprintf(m->database, sizeof m->database, "%s", character.motion_database);
    }
    const asset::AssetRegistry& registry = cache.registry();
    anim::MotionDatabase settings;
    u64 key = 1469598103934665603ull;
    asset::Guid guid;
    if (m->database[0] && asset::parse_guid(m->database, guid)) {
        if (const asset::AssetRecord* r = registry.find(guid)) {
            char path[300];
            registry.source_path(*r, path, sizeof path);
            asset::load_data_asset(path, settings);
            key ^= static_cast<u64>(r->source_time);
        }
    }
    // The clips, each with whether it loops.
    std::vector<const anim::Clip*> clips;
    std::vector<char> loops;
    auto add_set = [&](const char* anim_guid, i32 only, bool looping, bool by_name) {
        const ClipSet* set = cache.clips(mesh.mesh, anim_guid);
        if (!set) return;
        key = (key ^ static_cast<u64>(set->modified) ^ hash64(anim_guid)) * 1099511628211ull;
        for (u32 c = 0; c < set->count && clips.size() < 64; ++c) {
            if (only >= 0 && c != static_cast<u32>(only)) continue;
            clips.push_back(&set->clips[c]);
            loops.push_back(by_name ? static_cast<char>(!contains_word(set->clips[c].name, {"start", "stop", "turn", "pivot", "land", "jump"})) : static_cast<char>(looping));
        }
    };
    if (settings.clips.count) {
        for (const anim::MotionClip& c : settings.clips) add_set(c.animation[0] ? c.animation : mesh.mesh, static_cast<i32>(c.clip), c.looping, false);
    } else if (asset::parse_guid(mesh.mesh, guid)) {
        const asset::AssetRecord* own = registry.find(guid);
        for (usize i = 0; own && i < registry.count(); ++i) {
            const asset::AssetRecord& r = registry.get(i);
            if (r.type != asset::AssetType::Mesh || std::strcmp(r.folder, own->folder) != 0 || r.guid == own->guid) continue;
            if (!contains_word(r.name, {"idle", "walk", "run", "jog", "sprint", "strafe", "turn", "start", "stop", "pivot", "locomotion"})) continue;
            char id[33];
            asset::to_string(r.guid, id);
            add_set(id, -1, true, true);
        }
    }
    if (m->key == key && m->key != 0) return m->ok ? m : nullptr;
    m->key = key;
    m->settings = settings;
    bool flags[64] = {};
    for (usize i = 0; i < loops.size(); ++i) flags[i] = loops[i] != 0;
    m->ok = m->data.build(*model.model, clips.data(), flags, static_cast<u32>(clips.size()), settings, -radians(facing_yaw(mesh, &model)));
    if (m->ok) LSD_LOG_INFO("anim", "Motion matching: %u clips, %zu frames", m->data.clip_count, m->data.frames.size());
    else if (clips.empty()) LSD_LOG_WARN("anim", "Motion matching: no locomotion clips (put Idle / Walk / Run... FBX files next to the character, or list them in a Motion Database)");
    for (State& s : states_) {  // what was playing indexed the old build
        if (s.motion_data == &m->data) s.motion = anim::MotionState{};
    }
    return m->ok ? m : nullptr;
}

const anim::Pose* AnimationPlayer::pose(world::Entity e) const {
    for (const State& s : states_) {
        if (s.used && s.entity == e) return &s.pose;
    }
    return nullptr;
}

void AnimationPlayer::reset() {
    montages_.clear();
    finished_montages_.clear();
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
f32 AnimationPlayer::play_montage(const world::World& world, SkinnedCache& cache, world::Entity e, const char* animation, f32 rate, const char* from_bone, f32 blend,
                                  bool root_motion) {
    world::Entity mesh_entity = world.alive(e) && world.get<world::SkeletalMesh>(e) ? e : world::Entity{};
    for (u32 slot = 0; slot < world.slot_count() && !mesh_entity.valid(); ++slot) {  // a Character's mesh under it
        const world::Entity c = world.entity_at_slot(slot);
        if (c.valid() && world.parent(c) == e && world.get<world::SkeletalMesh>(c)) mesh_entity = c;
    }
    const auto* mesh = mesh_entity.valid() ? world.get<world::SkeletalMesh>(mesh_entity) : nullptr;
    if (!mesh) return 0.0f;
    char guid[33] = "";
    for (usize i = 0; i < cache.registry().count() && !guid[0]; ++i) {
        const asset::AssetRecord& r = cache.registry().get(i);
        if (std::strcmp(r.name, animation) == 0) asset::to_string(r.guid, guid);
    }
    const ClipSet* set = guid[0] ? cache.clips(mesh->mesh, guid) : nullptr;
    if (!set || set->count == 0 || set->clips[0].duration <= 0.0f) return 0.0f;
    stop_montage(e, 0.0f);  // one at a time per object (Unreal's default slot)
    Montage m;
    m.owner = e;
    m.mesh = mesh_entity;
    std::snprintf(m.animation, sizeof m.animation, "%s", guid);
    std::snprintf(m.from_bone, sizeof m.from_bone, "%s", from_bone ? from_bone : "");
    m.rate = rate > 0.0f ? rate : 1.0f;
    m.blend = blend > 0.0f ? blend : 0.0f;
    m.root_motion = root_motion;
    montages_.push_back(m);
    return set->clips[0].duration / m.rate;
}

void AnimationPlayer::stop_montage(world::Entity e, f32 blend) {
    for (usize i = montages_.size(); i-- > 0;) {
        if (montages_[i].owner != e) continue;
        if (blend > 0.0f) {
            montages_[i].fade_speed = 1.0f / blend;
        } else {
            finished_montages_.push_back(e);
            montages_.erase(montages_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

bool AnimationPlayer::montage_playing(world::Entity e) const {
    for (const Montage& m : montages_) {
        if (m.owner == e && m.fade_speed == 0.0f) return true;
    }
    return false;
}

bool AnimationPlayer::take_root_motion(world::Entity e, Vec3& moved) {
    for (Montage& m : montages_) {
        if (m.owner != e || !m.root_motion) continue;
        moved = m.moved;
        m.moved = {};
        return true;
    }
    return false;
}

void AnimationPlayer::take_finished_montages(std::vector<world::Entity>& out) {
    out.insert(out.end(), finished_montages_.begin(), finished_montages_.end());
    finished_montages_.clear();
}

void AnimationPlayer::apply_montage(const world::World& world, State& state, const SkinnedEntry& entry) {
    for (usize i = 0; i < montages_.size(); ++i) {
        Montage& m = montages_[i];
        const auto* mesh = m.mesh == state.entity ? world.get<world::SkeletalMesh>(state.entity) : nullptr;
        if (!mesh || !cache_ || !entry.model) continue;
        const ClipSet* set = cache_->clips(mesh->mesh, m.animation);
        const anim::Clip* clip = set && set->count ? &set->clips[0] : nullptr;
        if (!clip || clip->duration <= 0.0f) continue;
        m.time += dt_ * m.rate;
        if (m.fade_speed > 0.0f) m.fade -= dt_ * m.fade_speed;
        if (m.time >= clip->duration || m.fade <= 0.0f) {  // played out (or faded): back to what plays under it
            finished_montages_.push_back(m.owner);
            montages_.erase(montages_.begin() + static_cast<std::ptrdiff_t>(i));
            --i;
            continue;
        }
        f32 w = m.fade;
        if (m.blend > 0.0f) {
            const f32 in = m.time / m.blend, out = (clip->duration - m.time) / m.blend;
            w = std::fmin(w, std::fmin(in < 1.0f ? in : 1.0f, out < 1.0f ? out : 1.0f));
        }
        state.pose.layer(*entry.model, clip, m.time / clip->duration, w, m.from_bone[0] ? entry.model->find_joint(m.from_bone) : -1);
        if (m.root_motion) {  // the root's travel since last frame moves the object; the mesh stays on it
            const Vec3 offset = state.pose.strip_root(*entry.model);
            if (m.started) m.moved = m.moved + transform_vector(skeletal_matrix(world, state.entity, *mesh, &entry), offset - m.last);
            m.last = offset;
            m.started = true;
        }
    }
}

void AnimationPlayer::apply_ik(world::World& world, State& state, const SkinnedEntry& entry) {
    apply_montage(world, state, entry);  // montages over the sampled pose first, IK on top of both
    const anim::SkinnedModel& model = *entry.model;
    const auto* mesh = world.get<world::SkeletalMesh>(state.entity);
    const Mat4 to_world = mesh ? skeletal_matrix(world, state.entity, *mesh, &entry) : world.world_matrix(state.entity);
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
        out = skeletal_matrix(world, e, *mesh, model) * without_scale(p->joint_model(static_cast<u32>(j))) * trs(s.position, s.rotation, Vec3{1, 1, 1});
        return true;
    }
    // No socket of that name: a bone of that name (Unreal attaches to either).
    const SkinnedEntry* model = const_cast<SkinnedCache&>(cache).get(mesh->mesh);
    const i32 j = model && model->model ? model->model->find_joint(name) : -1;
    if (j < 0) return false;
    out = skeletal_matrix(world, e, *mesh, model) * without_scale(p->joint_model(static_cast<u32>(j)));
    return true;
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
    cache_ = &cache;
    dt_ = dt;
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
        const Mat4 m = world.world_matrix(e);
        const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};

        // Motion Matching: the Character (this object or a parent) asks for it; it picks the frames.
        const world::Character* character = nullptr;
        world::Entity owner;
        for (world::Entity p = e; p.valid() && !character; p = world.parent(p)) {
            if (const auto* c = world.get<world::Character>(p); c && c->motion_matching) character = c, owner = p;
        }
        if (const Motion* motion = character ? motion_for(cache, *mesh, *model, *character) : nullptr) {
            anim::MotionInput input;
            input.facing = input.desired_facing = std::atan2(m.at(1, 0), m.at(0, 0));  // standing still where it faces
            for (const MotionInputs& mi : motion_inputs_) input = mi.character == owner ? mi.input : input;
            state->motion_data = &motion->data;
            anim::BlendInput* inputs = state->inputs;
            const u32 n = anim::step_motion(motion->data, motion->settings, input, state->motion, mesh->playing ? dt * mesh->play_rate : 0.0f, inputs);
            state->input_count = n;
            state->pose.sample_blend(*model->model, inputs, n);
            apply_ik(world, *state, *model);
            update_morphs(*state, *mesh, *model);
            continue;
        }
        state->motion_data = nullptr;

        // Animation Graph: states, blend spaces and crossfades pick the clips.
        if (const anim::AnimGraph* graph = mesh->anim_graph[0] ? cache.graph(mesh->anim_graph) : nullptr) {
            const ClipSet* set = cache.clips(mesh->mesh, graph->animation[0] ? graph->animation : mesh->animation);
            anim::BlendInput* inputs = state->inputs;
            const u32 n = anim::step_graph(*graph, set ? set->clips : nullptr, set ? set->count : 0, state->graph, mesh->playing ? dt * mesh->play_rate : 0.0f, inputs);
            state->input_count = n;
            state->pose.sample_blend(*model->model, inputs, n);
            apply_ik(world, *state, *model);
            update_morphs(*state, *mesh, *model);
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
        apply_ik(world, *state, *model);
        f32 ratio = clip && clip->duration > 0.0f ? state->time / clip->duration : 0.0f;  // as Pose::sample does
        ratio = mesh->looping ? ratio - std::floor(ratio) : (ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio);
        state->inputs[0] = {clip, ratio, 1.0f};
        state->input_count = clip ? 1 : 0;
        update_morphs(*state, *mesh, *model);

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
