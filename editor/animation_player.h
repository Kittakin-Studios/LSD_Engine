#pragma once
// Plays the SkeletalMesh components: advances each one's clip time (or steps its Animation Graph),
// samples its pose for drawing, and in Play fires the animation's notifies (Play Sound at the
// object, like Unreal's AnimNotify).
#include "skinned_cache.h"
#include "sound_player.h"

#include "lsd/world/world.h"

#include <vector>

namespace lsd::editor {

class AnimationPlayer {
public:
    // `sounds` only in Play (notifies are silent in the editor).
    void update(world::World& world, SkinnedCache& cache, f32 dt, SoundPlayer* sounds);
    const anim::Pose* pose(world::Entity e) const;  // nullptr until the object has been updated
    // A socket's world transform on e's skinned mesh (the mesh's .meta sockets); false if none.
    bool socket(const world::World& world, const SkinnedCache& cache, world::Entity e, const char* socket, Mat4& out);
    // Attach To Socket components onto their sockets (after update).
    void attach_to_sockets(world::World& world, const SkinnedCache& cache);
    void reset();  // every object back to time 0 and its graph's entry state (Play starts afresh)
    // An Animation Graph parameter on `e`'s skeletal mesh and on those of its children (a Character
    // with its mesh attached under it).
    void set_param(const world::World& world, world::Entity e, const char* name, f32 value);

    // IK for the next update (scripts call it every frame they want it): a bone reaches a world
    // point (two-bone) or looks at it (aim along `forward`, the bone's own axis).
    void add_ik(world::Entity e, const char* bone, Vec3 target, f32 weight, bool aim, Vec3 forward = {1, 0, 0});
    // In Play: finds the ground under a point (Foot IK); nullptr in the editor (no foot IK there).
    using GroundProbe = bool (*)(void* user, Vec3 from, f32 distance, Vec3& hit);
    void set_ground_probe(GroundProbe probe, void* user) {
        probe_ = probe;
        probe_user_ = user;
    }

private:
    struct State {
        world::Entity entity;
        anim::Pose pose;
        f32 time = 0.0f;
        anim::GraphState graph;
        u32 notify_clip = ~0u;  // graph: the strongest clip last frame and where it was
        f32 notify_ratio = 0.0f;
        bool used = false;
    };
    State states_[256];
    struct Ik {
        world::Entity entity;
        char bone[48];
        Vec3 target, forward;
        f32 weight;
        bool aim;
    };
    Ik iks_[64];
    struct Sockets {  // a mesh's sockets from its .meta, re-read when the file changes
        char guid[33];
        i64 modified;
        asset::AssetMeta meta;
    };
    std::vector<Sockets> sockets_;
    u32 ik_count_ = 0;
    GroundProbe probe_ = nullptr;
    void* probe_user_ = nullptr;
    void apply_ik(world::World& world, State& state, const anim::SkinnedModel& model);
};

} // namespace lsd::editor
