#include "animation_player.h"

#include <cmath>

namespace lsd::editor {

const anim::Pose* AnimationPlayer::pose(world::Entity e) const {
    for (const State& s : states_) {
        if (s.used && s.entity == e) return &s.pose;
    }
    return nullptr;
}

void AnimationPlayer::reset() {
    for (State& s : states_) s.time = 0.0f;
}

void AnimationPlayer::update(world::World& world, SkinnedCache& cache, f32 dt, SoundPlayer* sounds) {
    for (State& s : states_) {  // objects deleted or without the component any more
        if (s.used && (!world.alive(s.entity) || !world.get<world::SkeletalMesh>(s.entity))) s.used = false;
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
            }
        }
        if (!state) continue;  // more than 256 animated objects: the rest stay still
        const ClipSet* set = cache.clips(mesh->mesh, mesh->animation);
        const anim::Clip* clip = set && mesh->clip < set->count ? &set->clips[mesh->clip] : nullptr;
        const f32 before = state->time;
        if (clip && mesh->playing) state->time += dt * mesh->play_rate;
        state->pose.sample(*model->model, clip, state->time, mesh->looping);

        // Notifies whose time this frame's advance crossed (wrapping when looping).
        if (!sounds || !clip || clip->duration <= 0.0f || state->time == before) continue;
        const f32 duration = clip->duration;
        auto local = [&](f32 t) { return mesh->looping ? t - std::floor(t / duration) * duration : (t < 0.0f ? 0.0f : t > duration ? duration : t); };
        const f32 from = local(before), to = local(state->time);
        const bool wrapped = mesh->looping && to < from;  // went past the end and round again
        const Mat4 m = world.world_matrix(e);
        for (const asset::AnimNotify& n : set->meta.notifies) {
            if (n.clip != mesh->clip || !n.sound[0]) continue;
            const bool crossed = wrapped ? (n.time > from || n.time <= to) : (n.time > from && n.time <= to);
            if (crossed) sounds->play(n.sound, true, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        }
    }
}

} // namespace lsd::editor
