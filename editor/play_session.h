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
#include "lsd/asset/localization.h"
#include "lsd/input/input.h"
#include "lsd/audio/voice_io.h"
#include "lsd/net/net.h"
#include "lsd/net/voice.h"

#include <memory>
#include <string>
#include <unordered_map>
#include "lsd/physics/physics.h"
#include "lsd/script/script.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

namespace lsd::ui {
class UiContext;
}

namespace lsd::editor {

class SkinnedCache;

class AnimationPlayer;

// Multiplayer for one Play (Project Settings > Multiplayer): host or join, and how.
struct NetPlay {
    bool enabled = false;
    bool host = true;        // false: join `address`
    bool dedicated = false;  // a server without a player of its own (no avatar)
    char address[64] = "127.0.0.1";
    net::NetSettings settings;
    char avatar[64] = "Cube";  // how each player looks to the others: a mesh asset or a shape by name
    bool voice = false;          // voice chat (off in silent runs)
    bool open_mic = false;
    char push_to_talk_key[24] = "V";
    char talk_to_all_key[24] = "B";
};

class PlaySession {
public:
    PlaySession() = default;
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    // `travel`: a level change inside the same Play (lsd.open_level): game_instance / player_state are kept.
    void start(world::World& world, MeshCache& meshes, const asset::AssetRegistry& registry, Vec3 eye, f32 yaw, f32 pitch, bool travel = false);
    // Moves the player and simulated objects; writes their new transforms into `world` (Stop reverts them).
    void update(world::World& world, const input::InputFrame& frame, f32 dt);
    void stop();

    Vec3 feet() const { return feet_; }
    Vec3 eye() const { return view_eye_; }  // where the game is seen from: the head, the spring arm or a Camera
    Vec3 view_forward() const { return view_forward_; }
    f32 view_fov() const { return view_fov_; }  // radians; 0: the editor's
    f32 view_ortho() const { return view_ortho_; }  // an orthographic Camera's height in metres; 0: perspective
    // Looks through `camera` (invalid: the player again), cutting or blending over `blend_seconds`.
    void set_view(world::Entity camera, f32 blend_seconds = 0.0f);
    // Unreal's camera shake: strength 1 = 5 cm and about 1 degree of wobble, fading out over `seconds`.
    void camera_shake(f32 strength, f32 seconds, f32 frequency);
    // Timelines by asset name (lsd.play_timeline): several can play at once.
    bool play_timeline(const char* name, bool loop);
    void stop_timeline(const char* name);
    Vec3 forward() const;
    // A Camera (not Auto Activate) on the player's Character or a child of it: where the player's view sits.
    world::Entity player_camera(const world::World& world) const;
    u32 camera_view_override_ = 2;  // lsd.set_camera_view: 0 first, 1 third, 2 = the Camera's own View
    bool wired() const { return wired_; }  // true when a Character's input assets drive the player
    bool run_lua(const char* code) { return scripts_.run(code); }  // console `lua`; false when not playing
    // Multiplayer: set before start(); off by default (single player).
    void set_network(const NetPlay& play) { net_play_ = play; }
    const net::NetSession& network() const { return net_; }
    struct ChatLine {
        u32 from;
        char channel[16];
        char text[256];
    };
    const std::vector<ChatLine>& chat_log() const { return chat_; }
    bool send_chat(const char* channel, const char* text);  // this machine's player says something
    bool talking() const { return talking_; }               // voice chat: transmitting now
    // The first physics surface on from -> to (particle World collision); false if none or not playing.
    static bool segment_hit(void* user, Vec3 from, Vec3 to, Vec3& point, Vec3& normal);
    f32 voice_level(u32 player) const { return voice_.level(player); }
    // Vehicles and movers for scripts: 0 enter vehicle e, 1 exit, 2 the driven vehicle (entity value),
    // 3 ride transport e, 4 get off, 5 start mover e, 6 stop it, 7 restart it. 0 = refused / none.
    u32 ride_command(world::World& world, int what, world::Entity e);
    void set_sounds(SoundPlayer* sounds) { sounds_ = sounds; }
    // The Character's Speed, VerticalSpeed and InAir go to its Animation Graph; Lua sets others.
    void set_animations(AnimationPlayer* animations) { animations_ = animations; }
    void set_skinned(SkinnedCache* skinned) { skinned_ = skinned; }
    script::ScriptSystem& scripts() { return scripts_; }  // the debugger attaches here once
    const ai::NavMesh& navmesh() const { return nav_; }  // built when Play starts
    // The game's UI (documents opened by scripts and the Game Mode's HUD). Its actions call Lua.
    void set_ui(ui::UiContext* ui);
    bool ui_cursor() const { return ui_cursor_; }  // a menu wants the mouse
    // The player's settings Lua reads and changes; `changed` tells main.cpp to apply and save them.
    void set_user_settings(GameUserSettings* settings) { user_settings_ = settings; }
    void set_localization(asset::Localization* localization) { localization_ = localization; }
    void set_base_mix(const char* guid) { base_mix_ = guid; }  // Project Settings > Audio > Base Sound Mix (read at each start)
    bool take_settings_changed() { const bool c = settings_changed_; settings_changed_ = false; return c; }
    // A level lsd.open_level asked for (its file path), once; the caller stops, loads it and starts with travel = true.
    bool take_level_request(char* path, usize capacity);
    // Moves UI elements that follow objects (lsd.ui_follow) to where those are on screen.
    void place_ui_followers(const world::World& world, const Mat4& view_proj, const f32 viewport[4]);
    // The running nodes of the first AI Controller using behaviour tree `guid` (graph node ids), for
    // the graph window's live debugging.
    u32 active_bt_nodes(const char* guid, u32* out, u32 capacity) const;

private:
    struct Simulated {
        world::Entity entity;
        u32 body;
        Vec3 scale;
        Vec3 centre;  // bounds centre in the entity's local space (unscaled)
        bool kinematic = false;  // follows its Transform instead of writing to it
    };

    static constexpr u32 kMaxBodies = 8192;
    static constexpr u32 kMaxJoints = 1024;
    world::Entity joint_owners_[kMaxJoints];  // the object whose Physics Joint made each joint

    static bool script_path(void* user, const char* guid, char* out, usize capacity);
    const asset::AssetRecord* find_asset(asset::AssetType type, const char* name) const;
    void add_entity_bodies(const world::World& world, world::Entity e);
    u32 add_body(world::Entity e, const physics::BodyDesc& desc);
    world::Entity body_entity(u32 body) const;
    world::Entity create_spawn(world::World& world, const char* name, Vec3 position);
    std::unordered_map<std::string, std::string> prefab_text_;  // spawned prefabs' files, read once a session
    // Multiplayer: the session, avatars and what the network asked scripts to do.
    NetPlay net_play_;
    net::NetSession net_;
    std::vector<ChatLine> chat_;
    world::Entity my_avatar_;
    world::Entity make_avatar(world::World& world, const char* name, Vec3 at, bool mine);
    static world::Entity net_spawn(void* user, const char* asset, const char* name, Vec3 position, Vec3 rotation);
    void net_receive(world::World& world, f32 dt);
    void net_send(world::World& world, f32 dt);
    // Voice chat: the microphone, a decoder per speaker, playback.
    audio::VoiceIO voice_;
    std::unique_ptr<net::VoiceEncoder> encoder_;
    std::unordered_map<u32, std::unique_ptr<net::VoiceDecoder>> decoders_;
    bool talking_ = false;
    void hear(u32 speaker, const char* channel, const u8* packet, u32 bytes);
    void talk(const input::InputFrame& frame);
    // Vehicles, transports and movers: objects moved by Play itself, on kinematic bodies.
    struct Moving {
        world::Entity entity, spline;
        u32 body = physics::kWorldBody;
        Vec3 scale{1, 1, 1}, centre{};  // the body's fit (see fitted_body)
        Vec3 start_position{}, start_rotation{}, world_scale{1, 1, 1}, spun{};
        f32 time = 0, distance = 0, direction = 1, wait = 0;
        f64 started_at = -1.0;  // play clock when its run began; -1: not yet
        bool running = false;
        f32 speed = 0, yaw = 0, fall = 0;  // a vehicle's motion
    };
    std::vector<Moving> moving_;
    world::Entity driving_, riding_;
    u64 nav_hash_ = 0;  // the navmesh's inputs when it was built (reused across Plays while equal)
    f64 play_clock_ = 0.0;
    bool interact_was_down_ = false;
    void add_moving(const world::World& world, world::Entity e, const CachedMesh* cached, bool solid);
    Moving* moving_of(world::Entity e);
    void restart_mover(world::World& world, Moving& m);
    bool step_mover(world::World& world, Moving& m, const world::Mover& mover, f32 dt, Vec3& at, Vec3& rotation);
    void drive(world::World& world, Moving& m, const world::Vehicle& v, const input::InputFrame& frame, input::Value move, f32 dt, Vec3& at);
    void update_rides(world::World& world, const input::InputFrame& frame, input::Value move, f32 dt);
    void place_rider(world::World& world);
    // Sub Levels in Play: load / unload (scripts and physics of what loads start at once).
    bool stream_sublevel(world::World& world, world::Entity root, bool load);
    void update_sublevels(world::World& world);
    void vehicle_view(const world::World& world);
    world::Entity spawn(const char* name, Vec3 position);  // during Play: bodies and scripts too
    void start_game_mode(world::World& world);
    void send_hits();
    void add_joints(const world::World& world, world::Entity root);  // all, if root is invalid
    u32 body_of(world::Entity e) const;  // kNoBody if it has none
    static bool action_guid(const asset::AssetRegistry& registry, const char* name, char* out);
    // Key rebinding (lsd.rebind): changes the live controls and records it in the player's settings.
    bool rebind(const char* action, const char* from, const char* to);
    void apply_input();  // context_ into input_, with each action's trigger

    physics::PhysicsWorld* physics_ = nullptr;
    script::ScriptSystem scripts_;
    const asset::AssetRegistry* registry_ = nullptr;
    world::World* world_ = nullptr;
    MeshCache* meshes_ = nullptr;
    world::Entity body_entities_[kMaxBodies];  // which object each physics body belongs to
    input::InputSystem input_;
    Vec3 view_eye_, view_forward_{1, 0, 0};
    f32 view_fov_ = 0.0f;
    f32 view_ortho_ = 0.0f;
    Vec3 blend_eye_, blend_forward_{1, 0, 0};  // the view a blend starts from
    f32 blend_fov_ = 0.0f, blend_time_ = 0.0f, blend_length_ = 0.0f;
    f32 shake_strength_ = 0.0f, shake_time_ = 0.0f, shake_length_ = 0.0f, shake_frequency_ = 12.0f, view_clock_ = 0.0f;
    world::Entity view_camera_;  // a Camera component the game looks through (lsd.set_view, Auto Activate)
    struct Projectile {
        world::Entity entity;
        Vec3 velocity;  // world space
        bool flying;
    };
    Projectile projectiles_[256];
    u32 projectile_count_ = 0;
    void update_movement(world::World& world, f32 dt);
    struct PlayingTimeline;
    PlayingTimeline* timelines_[8] = {};
    void update_timelines(world::World& world, f32 dt);
    void update_view(const world::World& world, f32 dt);
    void find_view(const world::World& world);  // the camera to look through, before blends and shakes
    input::InputMappingContext context_;  // the Character's controls, the player's rebinds applied
    char key_held_[32] = "";
    char key_pressed_[32] = "";  // went down this frame (lsd.key_pressed)
    char level_request_[300] = "";
    world::Character settings_;
    world::Entity character_;  // invalid for the default walker
    bool crouched_ = false;    // the capsule is at crouch_height
    bool swimming_ = false;    // the chest is in a Water Volume
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
    SkinnedCache* skinned_ = nullptr;
    ui::UiContext* ui_ = nullptr;
    GameUserSettings* user_settings_ = nullptr;
    asset::Localization* localization_ = nullptr;
    const char* base_mix_ = nullptr;
    struct UiFollower {
        i32 doc;
        char id[64];
        world::Entity entity;
        f32 height;
    };
    UiFollower ui_followers_[128];
    u32 ui_follower_count_ = 0;
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
