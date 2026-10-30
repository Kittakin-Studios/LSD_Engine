#pragma once
// Play-in-Editor on Jolt physics. On Start the level becomes physics bodies: meshes with collision and
// Blocking Volumes are static, RigidBody objects are simulated. The player is the first Character in
// the level, driven by the input assets picked on it; without one, a default first-person walker
// spawns at the Player Start (or the camera) with built-in WASD / mouse / Space controls.
// Kill Volumes and the World Settings' Kill Z send the player back to the start. Objects with a
// Script component run their Lua file (engine/script) after physics each frame; Trigger Volumes
// send them on_overlap_begin / on_overlap_end when the Character or a simulated object enters or leaves.
#include "mesh_cache.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/input/input.h"
#include "lsd/physics/physics.h"
#include "lsd/script/script.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

namespace lsd::editor {

class PlaySession {
public:
    PlaySession() = default;
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    void start(world::World& world, MeshCache& meshes, const asset::AssetRegistry& registry, Vec3 eye, f32 yaw, f32 pitch);
    // Moves the player and simulated objects; writes their new transforms into `world` (Stop reverts them).
    void update(world::World& world, const input::InputFrame& frame, f32 dt);
    void stop();

    Vec3 feet() const { return feet_; }
    Vec3 eye() const { return eye_; }  // the head, or behind it on the spring arm (camera_distance)
    Vec3 forward() const;
    bool wired() const { return wired_; }  // true when a Character's input assets drive the player
    bool run_lua(const char* code) { return scripts_.run(code); }  // console `lua`; false when not playing

private:
    struct Simulated {
        world::Entity entity;
        u32 body;
        Vec3 scale;
        Vec3 centre;  // bounds centre in the entity's local space (unscaled)
    };

    static constexpr u32 kMaxBodies = 8192;

    static bool script_path(void* user, const char* guid, char* out, usize capacity);
    const asset::AssetRecord* find_asset(asset::AssetType type, const char* name) const;
    void add_entity_bodies(const world::World& world, world::Entity e);
    u32 add_body(world::Entity e, const physics::BodyDesc& desc);
    world::Entity body_entity(u32 body) const;
    world::Entity create_spawn(world::World& world, const char* name, Vec3 position);
    world::Entity spawn(const char* name, Vec3 position);  // during Play: bodies and scripts too
    void start_game_mode(world::World& world);
    void send_hits();
    static bool action_guid(const asset::AssetRegistry& registry, const char* name, char* out);

    physics::PhysicsWorld* physics_ = nullptr;
    script::ScriptSystem scripts_;
    const asset::AssetRegistry* registry_ = nullptr;
    world::World* world_ = nullptr;
    MeshCache* meshes_ = nullptr;
    world::Entity body_entities_[kMaxBodies];  // which object each physics body belongs to
    input::InputSystem input_;
    world::Character settings_;
    world::Entity character_;  // invalid for the default walker
    bool wired_ = false;
    struct Overlap {
        world::Entity trigger, other;
    };
    void update_triggers(world::World& world);

    Simulated simulated_[1024];
    Overlap overlaps_[256];  // inside a Trigger Volume last frame
    u32 overlap_count_ = 0;
    u32 simulated_count_ = 0;
    Vec3 feet_, spawn_, eye_;
    f32 yaw_ = 0, pitch_ = 0, spawn_yaw_ = 0;
};

} // namespace lsd::editor
