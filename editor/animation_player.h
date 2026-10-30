#pragma once
// Plays the SkeletalMesh components: advances each one's clip time, samples its pose for drawing,
// and in Play fires the animation's notifies (Play Sound at the object, like Unreal's AnimNotify).
#include "skinned_cache.h"
#include "sound_player.h"

#include "lsd/world/world.h"

namespace lsd::editor {

class AnimationPlayer {
public:
    // `sounds` only in Play (notifies are silent in the editor).
    void update(world::World& world, SkinnedCache& cache, f32 dt, SoundPlayer* sounds);
    const anim::Pose* pose(world::Entity e) const;  // nullptr until the object has been updated
    void reset();  // every object back to time 0 (Play starts its animations from the beginning)

private:
    struct State {
        world::Entity entity;
        anim::Pose pose;
        f32 time = 0.0f;
        bool used = false;
    };
    State states_[256];
};

} // namespace lsd::editor
