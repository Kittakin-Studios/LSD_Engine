#pragma once
// Plays the SkeletalMesh components: advances each one's clip time (or steps its Animation Graph),
// samples its pose for drawing, and in Play fires the animation's notifies (Play Sound at the
// object, like Unreal's AnimNotify).
#include "skinned_cache.h"
#include "sound_player.h"

#include "lsd/animation/motion_matching.h"
#include "lsd/world/world.h"

#include <memory>
#include <vector>

namespace lsd::editor {

class AnimationPlayer {
public:
    explicit AnimationPlayer(rhi::Device& device) : device_(device) {}
    ~AnimationPlayer();
    // `sounds` only in Play (notifies are silent in the editor).
    void update(world::World& world, SkinnedCache& cache, f32 dt, SoundPlayer* sounds);
    const anim::Pose* pose(world::Entity e) const;  // nullptr until the object has been updated
    // Ragdoll (Simulate Physics): every joint's model-space transform from its bodies, drawn instead of the animation
    // from the next update on (clothes following it too); count = 0 gives the object back to its animation.
    void set_ragdoll_pose(world::Entity e, const Mat4* models, u32 count);
    // The Animation Graph state `e` is in ("" without a graph or before its first update), and the
    // crossfade's source state if one is running ("" otherwise).
    const char* graph_state(SkinnedCache& cache, const world::SkeletalMesh& mesh, world::Entity e, const char** fading_from = nullptr) const;
    // The vertices to skin for `e`: its own morphed copy when morph weights are set, else `model`'s.
    rhi::BufferHandle vertices(world::Entity e, rhi::BufferHandle model) const;
    // A socket's world transform on e's skinned mesh (the mesh's .meta sockets), or else a bone of
    // that name; false if neither.
    bool socket(const world::World& world, const SkinnedCache& cache, world::Entity e, const char* socket, Mat4& out);
    // Attach To Socket components onto their sockets (after update).
    void attach_to_sockets(world::World& world, const SkinnedCache& cache);
    // Outfits: each slot's mesh as a child piece (skinned: Leader Pose on the body; rigid: attached to
    // the slot's bone), made, swapped or removed as the slots change. Call before update().
    void update_outfits(world::World& world, SkinnedCache& cache);
    void reset();  // every object back to time 0 and its graph's entry state (Play starts afresh)
    // An Animation Graph parameter on `e`'s skeletal mesh and on those of its children (a Character
    // with its mesh attached under it).
    void set_param(const world::World& world, world::Entity e, const char* name, f32 value);
    // Motion Matching: where the input takes `character` (Play sets it every frame; else it stands still).
    void set_motion_input(world::Entity character, const anim::MotionInput& input);
    // What motion matching plays on `e` ("Anim_Walk at 0.43 s"), or false if it does not run there.
    bool motion_readout(world::Entity e, char* out, usize capacity) const;
    // The pose-search debugger: the frames nearest its last search ("Anim_Walk 0.43 s: 0.12 pose + 0.30 path"), nearest first.
    u32 motion_candidates(world::Entity e, char (*lines)[96], u32 capacity) const;

    // Montages (Unreal's Play Montage on a slot): an animation file's first clip played once over
    // what `e` (or its skeletal mesh child) plays, from `from_bone` down (empty: the whole body), blended
    // in and out over `blend` seconds. Returns how long it plays (0: no such animation).
    // `root_motion`: the clip's root movement moves the object (take_root_motion) instead of sliding the mesh.
    f32 play_montage(const world::World& world, SkinnedCache& cache, world::Entity e, const char* animation, f32 rate, const char* from_bone, f32 blend,
                     bool root_motion = false, const char* slot = "Default");
    void stop_montage(world::Entity e, f32 blend, const char* slot = "");  // blends out from where it is; slot "": every slot
    bool montage_playing(world::Entity e, const char* slot = "") const;     // slot "": any
    // Objects whose montage ended (played out or stopped) since the last call: Play sends them on_montage_ended.
    void take_finished_montages(std::vector<world::Entity>& out);
    // How far a root-motion montage moved `e` since the last call (world space, metres); false when none plays.
    // Also a Skeletal Mesh with Root Motion on: `e` is its Character (this object or a parent).
    bool take_root_motion(world::Entity e, Vec3& moved);
    // Play: root motion of objects that are not `player` (whose Character takes it) moves the object itself.
    void apply_root_motion(world::World& world, world::Entity player);
    // Animation curve `name` where `e` (or the Skeletal Mesh below it) plays now, blended like the pose; false if no clip has it.
    bool curve_value(const world::World& world, const SkinnedCache& cache, world::Entity e, const char* name, f32& value) const;

    // IK for the next update (scripts call it every frame they want it): a bone reaches a world
    // point (two-bone) or looks at it (aim along `forward`, the bone's own axis).
    void add_ik(world::Entity e, const char* bone, Vec3 target, f32 weight, bool aim, Vec3 forward = {1, 0, 0});
    // In Play: finds the ground under a point (Foot IK); nullptr in the editor (no foot IK there).
    using GroundProbe = bool (*)(void* user, Vec3 from, f32 distance, Vec3& hit, Vec3& normal);
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
        anim::BlendInput inputs[anim::kMaxBlendInputs];  // what was sampled this frame (morph curves follow it)
        u32 input_count = 0;
        f32 layer_time[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // the Animation Graph's layers: seconds into each clip
        rhi::BufferHandle morphed;  // bind pose + morph targets: the GPU's output (kept), or a CPU copy rebuilt per change
        u64 morph_key = 0;          // 0: no weights set, draw the model's own vertices
        u32 morph_vertex_count = 0;  // the GPU output's size (0: a CPU copy)
        std::vector<Mat4> ragdoll;   // set_ragdoll_pose: the joints' model-space transforms, while it is a ragdoll
        f32 pelvis = 0.0f;           // Foot IK: how far the pelvis is lowered now (eased toward the ground's wish)
        f32 foot_lift[2] = {0.0f, 0.0f};  // and each foot's height change
        bool used = false;
        anim::MotionState motion;
        const anim::MotionData* motion_data = nullptr;  // what `motion` indexes (a rebuild restarts it)
        // Root motion (SkeletalMesh::root_motion): who it moves, and the root's travel not yet taken.
        world::Entity owner;
        bool root_on = false, root_started = false;
        const anim::Clip* root_clip = nullptr;  // the strongest clip last frame and where it was (a loop wrapping)
        f32 root_ratio = 0.0f;
        Vec3 root_last, root_step, root_moved;
    };
    void step_root_motion(const world::World& world, State& state, const world::SkeletalMesh& mesh, const SkinnedEntry& entry, const anim::BlendInput* inputs, u32 count);
    // Motion Databases built for a skeleton, rebuilt when a clip file or the database changes.
    struct Motion {
        char mesh[33];
        char database[33];  // "" = the clips found next to the mesh
        u64 key = 0;
        anim::MotionDatabase settings;
        anim::MotionData data{default_allocator()};
        bool ok = false;
    };
    std::vector<std::unique_ptr<Motion>> motions_;
    struct Montage {
        world::Entity owner;  // who asked (gets on_montage_ended)
        world::Entity mesh;   // the object with the Skeletal Mesh
        char animation[33];   // the animation file's GUID
        char from_bone[48];
        char slot[32];  // Unreal's montage slot: a new montage replaces only the one in its slot
        f32 time = 0.0f, rate = 1.0f, blend = 0.2f;
        f32 fade = 1.0f, fade_speed = 0.0f;  // stop_montage: fades to 0 at fade_speed per second
        bool root_motion = false;
        bool started = false;  // root motion: `last` holds the previous frame's root offset
        Vec3 last;
        Vec3 moved;            // world-space movement not yet taken by Play
    };
    std::vector<Montage> montages_;
    std::vector<world::Entity> finished_montages_;
    SkinnedCache* cache_ = nullptr;  // during update (montages read their clips)
    f32 dt_ = 0.0f;
    bool in_play_ = false;  // root motion moves objects only in Play
    void apply_montage(const world::World& world, State& state, const SkinnedEntry& entry);
    const Motion* motion_for(SkinnedCache& cache, const world::SkeletalMesh& mesh, const SkinnedEntry& model, const world::Character& character);
    struct MotionInputs {
        world::Entity character;
        anim::MotionInput input;
    };
    std::vector<MotionInputs> motion_inputs_;
    rhi::Device& device_;
    void update_morphs(State& state, const world::SkeletalMesh& mesh, const SkinnedEntry& model);
    // Each model's targets packed for the GPU (anim::pack_morphs), by its vertex buffer.
    // shortcut: a reloaded model's old entry stays until the player goes; drop entries on reload if reloads pile up.
    rhi::BufferHandle morph_data(const SkinnedEntry& model);
    struct MorphData {
        u64 vertices;
        rhi::BufferHandle buffer;
    };
    std::vector<MorphData> morph_data_;
    struct Follower {
        State* state;
        world::Entity entity;
    };
    std::vector<Follower> followers_;  // this update's meshes with a Leader Pose
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
    void apply_ik(world::World& world, State& state, const SkinnedEntry& model);
};

} // namespace lsd::editor
