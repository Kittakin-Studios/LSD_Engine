#pragma once
// Play-in-Editor on Jolt physics. On Start the level becomes physics bodies: meshes with collision and
// Blocking Volumes are static, RigidBody objects are simulated. The player is the first Character in
// the level, driven by the input assets picked on it; without one, a default first-person walker
// spawns at the Player Start (or the camera) with built-in WASD / mouse / Space controls.
// Kill Volumes and the World Settings' Kill Z send the player back to the start. Objects with a
// Script component run their Lua file (engine/script) after physics each frame; Trigger Volumes
// send them on_overlap_begin / on_overlap_end when the Character or a simulated object enters or leaves.
#include "editor_settings.h"
#include "mesh_cache.h"
#include "sound_player.h"

#include "lsd/ai/behavior_tree.h"
#include "lsd/ai/navigation.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/input/input.h"
#include "lsd/physics/physics.h"
#include "lsd/script/script.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

namespace lsd::ui {
class UiContext;
}

namespace lsd::editor {

class AnimationPlayer;

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
    void set_sounds(SoundPlayer* sounds) { sounds_ = sounds; }
    // The Character's Speed, VerticalSpeed and InAir go to its Animation Graph; Lua sets others.
    void set_animations(AnimationPlayer* animations) { animations_ = animations; }
    const ai::NavMesh& navmesh() const { return nav_; }  // built when Play starts
    // The game's UI (documents opened by scripts and the Game Mode's HUD). Its actions call Lua.
    void set_ui(ui::UiContext* ui);
    bool ui_cursor() const { return ui_cursor_; }  // a menu wants the mouse
    // The player's settings Lua reads and changes; `changed` tells main.cpp to apply and save them.
    void set_user_settings(GameUserSettings* settings) { user_settings_ = settings; }
    bool take_settings_changed() { const bool c = settings_changed_; settings_changed_ = false; return c; }
    // The running nodes of the first AI Controller using behaviour tree `guid` (graph node ids), for
    // the graph window's live debugging.
    u32 active_bt_nodes(const char* guid, u32* out, u32 capacity) const;

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
    void add_joints(const world::World& world, world::Entity root);  // all, if root is invalid
    u32 body_of(world::Entity e) const;  // kNoBody if it has none
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
    struct Follower {
        world::Entity entity, spline;
        f32 distance;
        f32 direction;  // +1 forward, -1 back (Ping-Pong)
        u32 body;       // kinematic body, or physics::kWorldBody when it has no collision
        Vec3 scale, centre;
    };
    SoundPlayer* sounds_ = nullptr;
    AnimationPlayer* animations_ = nullptr;
    ui::UiContext* ui_ = nullptr;
    GameUserSettings* user_settings_ = nullptr;
    bool settings_changed_ = false;
    bool ui_cursor_ = false;
    // Nav Agents walking on the navmesh.
    ai::NavMesh nav_;
    struct Agent {
        world::Entity entity;
        i32 id;
    };
    Agent agents_[ai::kMaxAgents];
    u32 agent_count_ = 0;
    i32 agent_of(world::Entity e);  // adds it on first use; -1 if it cannot walk here
    void update_agents(world::World& world, f32 dt);
    // AI Controllers: a behaviour tree, a blackboard and perception each.
    struct Brain {
        PlaySession* session;
        world::Entity entity;
        char tree_guid[33];
        const ai::BehaviorTree* tree;  // nullptr: no (valid) tree, perception only
        ai::BtRunner runner;
        ai::Blackboard board;
        f32 dt;
    };
    static constexpr u32 kMaxBrains = 64;
    Brain* brains_ = nullptr;  // kMaxBrains, while playing
    u32 brain_count_ = 0;
    struct TreeEntry {
        char guid[33];
        ai::BehaviorTree tree;
        bool ok;
    };
    TreeEntry* trees_[16] = {};
    u32 tree_count_ = 0;
    struct Noise {
        Vec3 at;
        f32 loudness;
    };
    Noise noises_[16];
    u32 noise_count_ = 0;
    Brain* brain_of(world::Entity e);  // adds it on first use if it has an AI Controller
    const ai::BehaviorTree* tree_for(const char* guid);
    void update_brains(world::World& world, f32 dt);
    struct Emitter {
        world::Entity entity;
        u32 voice;
    };
    Emitter emitters_[256];
    u32 emitter_count_ = 0;
    Follower followers_[256];
    u32 follower_count_ = 0;
    void update_followers(world::World& world, f32 dt);
    void update_audio_volumes(const world::World& world, f32 dt);
    f32 reverb_now_ = 0.0f, room_now_ = 0.5f;
    u32 overlap_count_ = 0;
    u32 simulated_count_ = 0;
    Vec3 feet_, spawn_, eye_;
    f32 yaw_ = 0, pitch_ = 0, spawn_yaw_ = 0;
};

} // namespace lsd::editor
