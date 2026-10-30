#pragma once
// Play-in-Editor: a first-person player spawned at the level's Player Start (or the editor camera)
// that walks, jumps and collides with StaticMeshes that have collision on and Blocking Volumes;
// Kill Volumes and the World Settings' Kill Z send it back to the start.
// shortcut: the player is a box colliding with each mesh's world-space bounding box until Jolt
// (milestone 4) brings real collision shapes and the Character Movement component.
#include "mesh_cache.h"

#include "lsd/world/world.h"

namespace lsd::editor {

struct PlayInput {
    bool forward, back, left, right, jump, sprint;
    f32 look_x, look_y;  // mouse movement in pixels
};

class PlaySession {
public:
    // Spawns at the first PlayerStart; without one, at `eye` looking along `yaw`/`pitch`.
    void start(const world::World& world, Vec3 eye, f32 yaw, f32 pitch);
    void update(const world::World& world, MeshCache& meshes, const PlayInput& input, f32 dt);

    Vec3 feet() const { return position_; }
    Vec3 eye() const { return position_ + Vec3{0, 0, eye_height_}; }
    Vec3 forward() const;
    f32 yaw() const { return yaw_; }
    f32 pitch() const { return pitch_; }

private:
    Vec3 position_;  // feet
    Vec3 velocity_;
    Vec3 spawn_;
    f32 yaw_ = 0, pitch_ = 0, spawn_yaw_ = 0;
    f32 eye_height_ = 1.7f, walk_speed_ = 5.0f, jump_speed_ = 5.0f;
    bool on_ground_ = false;
};

} // namespace lsd::editor
