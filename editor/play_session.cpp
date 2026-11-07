#include "play_session.h"

#include "animation_player.h"
#include "debug_draw.h"
#include "skinned_cache.h"
#include "timeline_player.h"
#include "behavior.h"

#include "lsd/foundation/profile.h"

#include "lsd/ui/ui.h"
#include "navigation.h"
#include "graph_editor.h"
#include "paper2d.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_scancode.h>

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/world/level.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"
#include "lsd/world/timeline.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace lsd::editor {

namespace {

std::string json_string(const char* text);  // a JSON string literal (with Multiplayer, below)

// Action ids of the built-in controls used when the level has no Character (not real asset GUIDs).
constexpr char kDefaultMove[] = "000000000000000000000000000d0001";
constexpr char kDefaultLook[] = "000000000000000000000000000d0002";
constexpr char kDefaultJump[] = "000000000000000000000000000d0003";

input::InputMappingContext default_controls() {
    input::InputMappingContext context;
    auto add = [&](const char* action, const char* key, u32 axis, f32 scale) {
        input::KeyMapping m;
        std::snprintf(m.action, sizeof m.action, "%s", action);
        std::snprintf(m.key, sizeof m.key, "%s", key);
        m.axis = axis;
        m.scale = scale;
        context.mappings.push_back(m);
    };
    add(kDefaultMove, "W", 1, 1.0f);
    add(kDefaultMove, "S", 1, -1.0f);
    add(kDefaultMove, "D", 0, 1.0f);
    add(kDefaultMove, "A", 0, -1.0f);
    add(kDefaultMove, "Gamepad Left Stick", 0, 1.0f);
    add(kDefaultLook, "Mouse XY", 0, 1.0f);
    add(kDefaultLook, "Gamepad Right Stick", 0, 12.0f);
    add(kDefaultJump, "Space", 0, 1.0f);
    add(kDefaultJump, "Gamepad Face Bottom", 0, 1.0f);
    return context;
}

// A body fitted to a local box (lo..hi) under an entity's world matrix; the scale goes into the size.
physics::BodyDesc fitted_body(const Mat4& world, Vec3 lo, Vec3 hi, u32 shape, physics::Motion motion, Vec3& scale, Vec3& centre) {
    Vec3 t, r;
    decompose_trs(world, t, r, scale);
    centre = (lo + hi) * 0.5f;
    const Vec3 half = mul(hi - lo, scale) * 0.5f;
    physics::BodyDesc d;
    d.motion = motion;
    d.transform = translation(t) * rotation_euler(r) * translation(mul(centre, scale));
    if (shape == 1) {
        d.shape = physics::Shape::Sphere;
        d.half_extents = {half.x > half.y ? (half.x > half.z ? half.x : half.z) : (half.y > half.z ? half.y : half.z), 0, 0};
    } else if (shape == 2) {
        d.shape = physics::Shape::Capsule;
        const f32 radius = half.x > half.y ? half.x : half.y;
        d.half_extents = {radius, 0, half.z - radius > 0.01f ? half.z - radius : 0.01f};
    } else {
        d.half_extents = half;
    }
    return d;
}

constexpr Vec3 kUnitLo{-0.5f, -0.5f, 0.0f}, kUnitHi{0.5f, 0.5f, 1.0f};  // volumes and meshless bodies

// Is the point inside the volume's unit box (under its world matrix)?
bool inside_volume(const world::World& world, world::Entity volume, Vec3 point) {
    const Vec3 p = transform_point(affine_inverse(world.world_matrix(volume)), point);
    return p.x >= kUnitLo.x && p.x <= kUnitHi.x && p.y >= kUnitLo.y && p.y <= kUnitHi.y && p.z >= kUnitLo.z && p.z <= kUnitHi.z;
}

template <typename T>
bool load_asset(const asset::AssetRegistry& registry, const char* guid_text, T& out) {
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (!r) return false;
    char path[300];
    registry.source_path(*r, path, sizeof path);
    return asset::load_data_asset(path, out);
}

// The script host's shape numbers (0 sphere, 1 box, 2 capsule) as a physics query shape.
physics::QueryShape query_shape(int shape, Vec3 size) {
    physics::QueryShape q;
    q.kind = shape == 1 ? physics::QueryShape::Box : shape == 2 ? physics::QueryShape::Capsule : physics::QueryShape::Sphere;
    q.radius = size.x;
    q.half_height = size.y;
    q.half_extents = size;
    return q;
}

} // namespace

PlaySession::~PlaySession() { stop(); }

i32 PlaySession::agent_of(world::Entity e) {
    for (u32 i = 0; i < agent_count_; ++i) {
        if (agents_[i].entity == e) return agents_[i].id;
    }
    const auto* agent = world_ ? world_->get<world::NavAgent>(e) : nullptr;
    if (!agent || agent_count_ == ai::kMaxAgents) return -1;
    const Mat4 m = world_->world_matrix(e);
    const i32 id = nav_.add_agent({m.at(0, 3), m.at(1, 3), m.at(2, 3)}, agent->radius, agent->height, agent->max_speed);
    if (id < 0) {
        LSD_LOG_WARN("play", "%s: Nav Agent is not on the navmesh", world_->name(e));
        return -1;
    }
    agents_[agent_count_++] = {e, id};
    return id;
}

const ai::BehaviorTree* PlaySession::tree_for(const char* guid) {
    if (!guid[0]) return nullptr;
    for (u32 i = 0; i < tree_count_; ++i) {
        if (std::strcmp(trees_[i]->guid, guid) == 0) return trees_[i]->ok ? &trees_[i]->tree : nullptr;
    }
    if (tree_count_ == sizeof trees_ / sizeof trees_[0]) return nullptr;
    TreeEntry* t = trees_[tree_count_++] = new TreeEntry();
    std::snprintf(t->guid, sizeof t->guid, "%s", guid);
    asset::Guid id;
    const asset::AssetRecord* r = asset::parse_guid(guid, id) ? registry_->find(id) : nullptr;
    char path[300], error[256] = "";
    if (r) registry_->source_path(*r, path, sizeof path);
    t->ok = r && load_behavior_tree(path, t->tree, error, sizeof error);
    if (!t->ok) LSD_LOG_ERROR("ai", "Behaviour tree %s: %s", r ? r->name : guid, error[0] ? error : "not found");
    return t->ok ? &t->tree : nullptr;
}

PlaySession::Brain* PlaySession::brain_of(world::Entity e) {
    if (!brains_ || !world_) return nullptr;
    for (u32 i = 0; i < brain_count_; ++i) {
        if (brains_[i].entity == e) return &brains_[i];
    }
    const auto* controller = world_->alive(e) ? world_->get<world::AIController>(e) : nullptr;
    if (!controller || brain_count_ == kMaxBrains) return nullptr;
    Brain& b = brains_[brain_count_++];
    b = Brain{};
    b.session = this;
    b.entity = e;
    std::snprintf(b.tree_guid, sizeof b.tree_guid, "%s", controller->behavior_tree);
    b.tree = tree_for(b.tree_guid);
    return &b;
}

u32 PlaySession::active_bt_nodes(const char* guid, u32* out, u32 capacity) const {
    for (u32 i = 0; i < brain_count_; ++i) {
        if (std::strcmp(brains_[i].tree_guid, guid) == 0) return brains_[i].runner.active(out, capacity);
    }
    return 0;
}

// Perception, then each AI Controller's behaviour tree.
void PlaySession::update_brains(world::World& world, f32 dt) {
    if (!brains_) return;
    const reflect::TypeInfo& type = reflect::type_of<world::AIController>();
    for (u32 i = 0; i < world.count(type); ++i) brain_of(world.owner(type, i));  // new ones (spawned) too
    const Vec3 player_eye = feet_ + Vec3{0, 0, settings_.eye_height};
    ai::BtHost host;
    host.move_to = [](void* user, Vec3 target, f32) {
        Brain& b = *static_cast<Brain*>(user);
        const i32 agent = b.session->agent_of(b.entity);
        return agent >= 0 && b.session->nav_.move_agent(agent, target);
    };
    host.moving = [](void* user) {
        Brain& b = *static_cast<Brain*>(user);
        return b.session->nav_.agent_moving(b.session->agent_of(b.entity));
    };
    host.stop = [](void* user) {
        Brain& b = *static_cast<Brain*>(user);
        b.session->nav_.stop_agent(b.session->agent_of(b.entity));
    };
    host.position = [](void* user) {
        Brain& b = *static_cast<Brain*>(user);
        const Mat4 m = b.session->world_->world_matrix(b.entity);
        return Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    };
    host.random_point = [](void* user, Vec3 around, f32 radius, Vec3& out) { return static_cast<Brain*>(user)->session->nav_.random_point(around, radius, out); };
    host.run_script = [](void* user, const char* function, f32) {
        Brain& b = *static_cast<Brain*>(user);
        return b.session->scripts_.call_task(b.entity, function, b.dt);
    };
    for (u32 i = 0; i < brain_count_;) {
        Brain& b = brains_[i];
        const auto* controller = world.alive(b.entity) ? world.get<world::AIController>(b.entity) : nullptr;
        if (!controller) {  // destroyed by a script
            brains_[i] = brains_[--brain_count_];
            continue;
        }
        const Mat4 m = world.world_matrix(b.entity);
        const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        // Sight: in range, inside the field of view, nothing solid in between.
        const Vec3 eye = at + Vec3{0, 0, 1.6f};
        const Vec3 to_player = player_eye - eye;
        const f32 distance = length(to_player);
        bool sees = distance <= controller->sight_range && distance > 1e-3f;
        if (sees) {
            const Vec3 dir = to_player * (1.0f / distance);
            Vec3 forward = transform_vector(m, {1, 0, 0});
            forward.z = 0;
            const f32 flat = length(forward);
            const f32 half = radians(controller->sight_angle * 0.5f);
            sees = controller->sight_angle >= 360.0f || (flat > 1e-4f && dot(forward * (1.0f / flat), normalize(Vec3{dir.x, dir.y, 0})) >= std::cos(half));
            physics::RayHit hit;
            const f32 skip = 0.6f;  // past the agent's own body
            if (sees && distance > skip && physics_->raycast(eye + dir * skip, dir, distance - skip - 0.3f, hit)) sees = false;
        }
        if (sees) {
            b.board.set("CanSeePlayer", 1.0f);
            b.board.set("PlayerLocation", feet_);
        } else {
            b.board.clear("CanSeePlayer");
        }
        // Hearing.
        for (u32 n = 0; n < noise_count_; ++n) {
            const Vec3 d = noises_[n].at - at;
            const f32 reach = controller->hearing_range * noises_[n].loudness;
            if (dot(d, d) > reach * reach) continue;
            b.board.set("NoiseLocation", noises_[n].at);
            b.board.set("HeardNoise", 1.0f);
        }
        if (b.tree) {
            b.dt = dt;
            host.user = &b;
            b.runner.tick(*b.tree, b.board, host, dt);
        }
        ++i;
    }
    noise_count_ = 0;
}

// Moves the agents and writes them back to their objects (feet on the navmesh, facing their way).
void PlaySession::update_agents(world::World& world, f32 dt) {
    if (!nav_.valid()) return;
    nav_.update(dt);
    for (u32 i = 0; i < agent_count_;) {
        const Agent a = agents_[i];
        const auto* agent = world.alive(a.entity) ? world.get<world::NavAgent>(a.entity) : nullptr;
        if (!agent) {  // destroyed (or the component removed) by a script
            nav_.remove_agent(a.id);
            agents_[i] = agents_[--agent_count_];
            continue;
        }
        Vec3 t, rotation, scale;
        decompose_trs(world.world_matrix(a.entity), t, rotation, scale);
        const Vec3 velocity = nav_.agent_velocity(a.id);
        if (agent->face_movement && velocity.x * velocity.x + velocity.y * velocity.y > 0.01f) rotation = {0, 0, degrees(std::atan2(velocity.y, velocity.x))};
        world.set_world_matrix(a.entity, trs(nav_.agent_position(a.id), rotation, scale));
        ++i;
    }
}

void PlaySession::stop() {
    prefab_text_.clear();  // edits between sessions are read again
    net_.shutdown();  // tells the others first
    voice_.stop();
    encoder_.reset();
    decoders_.clear();
    talking_ = false;
    my_avatar_ = {};
    moving_.clear();
    driving_ = riding_ = {};
    play_clock_ = 0.0;
    interact_was_down_ = false;
    chat_.clear();
    if (ui_) ui_->close_all();
    ui_cursor_ = false;
    ui_follower_count_ = 0;
    scripts_.stop();
    nav_.clear_agents();  // the mesh stays for the next Play (rebuilt only if the level changed)
    agent_count_ = 0;
    delete[] brains_;
    brains_ = nullptr;
    brain_count_ = 0;
    for (u32 i = 0; i < tree_count_; ++i) delete trees_[i];
    tree_count_ = 0;
    if (sounds_) sounds_->stop_all_and_forget();
    emitter_count_ = 0;
    delete physics_;
    physics_ = nullptr;
    simulated_count_ = 0;
    projectile_count_ = 0;
    stop_timeline(nullptr);
    view_camera_ = {};
    overlap_count_ = 0;
    follower_count_ = 0;
    input_.clear();
}

world::Entity PlaySession::player_camera(const world::World& world) const {
    if (!world.alive(character_)) return {};
    const reflect::TypeInfo& type = reflect::type_of<world::Camera>();
    for (u32 i = 0; i < world.count(type); ++i) {
        if (static_cast<const world::Camera*>(world.at(type, i))->auto_activate) continue;
        const world::Entity e = world.owner(type, i);
        for (world::Entity p = e; p.valid(); p = world.parent(p)) {
            if (p == character_) return e;
        }
    }
    return {};
}

Vec3 PlaySession::forward() const {
    return {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_)};
}

bool PlaySession::script_path(void* user, const char* guid_text, char* out, usize capacity) {
    const auto& registry = *static_cast<const asset::AssetRegistry*>(user);
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (r && r->type == asset::AssetType::Graph) return graph_lua(registry, *r, out, capacity);  // visual scripts run as compiled Lua
    if (!r || r->type != asset::AssetType::Script) return false;
    registry.source_path(*r, out, capacity);
    return true;
}

// Scripts name Input Actions by asset name ("IA_Move"); the input system knows them by GUID.
bool PlaySession::action_guid(const asset::AssetRegistry& registry, const char* name, char* out) {
    usize hits[8];
    const usize n = registry.search(name, asset::AssetType::Data, nullptr, hits, 8);
    for (usize i = 0; i < n; ++i) {
        const asset::AssetRecord& r = registry.get(hits[i]);
        if (std::strcmp(r.name, name) == 0 && std::strcmp(r.data_type, "InputAction") == 0) {
            asset::to_string(r.guid, out);
            return true;
        }
    }
    const char* defaults[][2] = {{"Move", kDefaultMove}, {"Look", kDefaultLook}, {"Jump", kDefaultJump}};  // the built-in controls
    for (const auto& d : defaults) {
        if (std::strcmp(name, d[0]) == 0) {
            std::snprintf(out, 33, "%s", d[1]);
            return true;
        }
    }
    return false;
}

void PlaySession::apply_rebinds() {
    if (!user_settings_ || !registry_) return;
    for (const KeyBinding& b : user_settings_->key_bindings) {  // the player's rebound keys
        char guid[33];
        if (!action_guid(*registry_, b.action, guid)) continue;
        for (input::KeyMapping& m : context_.mappings) {
            if (std::strcmp(m.action, guid) == 0 && SDL_strcasecmp(m.key, b.from) == 0) std::snprintf(m.key, sizeof m.key, "%s", b.to);
        }
    }
}

// The newest change time of the Character's Mapping Context and its Input Actions' files.
i64 PlaySession::input_files_stamp() const {
    if (!registry_ || !wired_) return 0;
    i64 newest = 0;
    auto stamp = [&](const char* guid_text) {
        asset::Guid guid;
        const asset::AssetRecord* r = asset::parse_guid(guid_text, guid) ? registry_->find(guid) : nullptr;
        if (!r) return;
        char path[300];
        registry_->source_path(*r, path, sizeof path);
        std::error_code error;
        const i64 t = static_cast<i64>(std::filesystem::last_write_time(path, error).time_since_epoch().count());
        if (!error && t > newest) newest = t;
    };
    stamp(settings_.mapping_context);
    for (const input::KeyMapping& m : context_.mappings) stamp(m.action);
    return newest;
}

void PlaySession::apply_input() {
    input_.clear();
    input_.add_context(context_);
    for (const input::KeyMapping& m : context_.mappings) {  // each action's trigger (Hold, Tap...) from its asset
        input::InputAction action;
        if (registry_ && load_asset(*registry_, m.action, action)) input_.configure(m.action, action);
    }
}

bool PlaySession::rebind(const char* action, const char* from, const char* to) {
    char guid[33];
    if (!registry_ || !action_guid(*registry_, action, guid)) {
        LSD_LOG_WARN("input", "lsd.rebind: no Input Action named '%s'", action);
        return false;
    }
    if (!input::is_key(to)) {
        LSD_LOG_WARN("input", "lsd.rebind: unknown key '%s'", to);
        return false;
    }
    bool found = false;
    for (input::KeyMapping& m : context_.mappings) {
        if (std::strcmp(m.action, guid) == 0 && SDL_strcasecmp(m.key, from) == 0) {
            std::snprintf(m.key, sizeof m.key, "%s", to);
            found = true;
        }
    }
    if (!found) return false;
    apply_input();
    if (user_settings_) {  // saved as original key -> new key, so the asset's key stays the reference
        auto& list = user_settings_->key_bindings;
        u32 i = 0;
        while (i < list.count && !(std::strcmp(list[i].action, action) == 0 && SDL_strcasecmp(list[i].to, from) == 0)) ++i;
        if (i == list.count) {
            KeyBinding b;
            std::snprintf(b.action, sizeof b.action, "%s", action);
            std::snprintf(b.from, sizeof b.from, "%s", from);
            if (!list.push_back(b)) LSD_LOG_WARN("input", "Too many rebound keys to save (32)");
        }
        if (i < list.count) {
            std::snprintf(list[i].to, sizeof list[i].to, "%s", to);
            if (SDL_strcasecmp(list[i].from, list[i].to) == 0) list.remove(i);  // back to the default
        }
        settings_changed_ = true;
    }
    return true;
}

void PlaySession::start(world::World& world, MeshCache& meshes, const asset::AssetRegistry& registry, Vec3 eye, f32 yaw, f32 pitch, bool travel) {
    stop();
    const u64 start_ns = SDL_GetTicksNS();  // how long Play takes to start (logged at the end)
    if (!travel) {
        scripts_.clear_persistent();  // a new Play: a fresh game instance
        std::snprintf(current_level_, sizeof current_level_, "*");
    }
    if (!net_play_.enabled || !net_play_.host) upnp_.stop();  // left or joined: the forward goes
    level_request_[0] = '\0';
    registry_ = &registry;
    world_ = &world;
    meshes_ = &meshes;
    physics_ = new physics::PhysicsWorld();
    physics_->set_gravity(world.settings().gravity);
    live_gravity_ = world.settings().gravity;
    start_game_mode(world);  // may spawn the player and a GameMode object first
    // Multiplayer: host or join before any body or script exists (clients make replicated bodies
    // kinematic; scripts may ask lsd.net_mode() in on_start).
    // After lsd.open_level (travel) the session starts again on the new level: the server hosts it,
    // clients (told by "__travel") join it again, so everyone arrives together.
    // shortcut: a full reconnect (a moment of "connecting"); keep the connection across levels when that pause matters
    if (net_play_.enabled) {
        net_.set_spawner(net_spawn, this);
        const bool ok = net_play_.host ? net_.host(world, net_play_.settings) : net_.join(world, net_play_.address, net_play_.settings);
        if (ok && net_play_.host && !net_play_.dedicated) {
            my_avatar_ = make_avatar(world, "Player 0", feet_, true);
            net_.set_owner(world, my_avatar_, net::kServerPlayer);
        }
        if (ok && net_play_.voice && voice_.start(!net_play_.dedicated)) encoder_ = std::make_unique<net::VoiceEncoder>();
    }
    if (world.settings().level_script[0]) {  // the Level Blueprint: a script on an object of its own
        std::snprintf(world.add<world::Script>(world.create("LevelScript"))->script, sizeof(world::Script::script), "%s", world.settings().level_script);
    }

    // Sub Levels: present at start only when they load at start and the player is near enough.
    if (!travel) {
        const reflect::TypeInfo& sub_type = reflect::type_of<world::SubLevel>();
        for (u32 i = 0; i < world.count(sub_type); ++i) {
            const world::Entity e = world.owner(sub_type, i);
            const auto* sub = static_cast<const world::SubLevel*>(world.at(sub_type, i));
            const Mat4 m = world.world_matrix(e);
            const bool near = sub->load_distance <= 0.0f || length(Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - feet_) < sub->load_distance;
            if (!sub->load_at_start || !near) world::unload_sublevel(world, e);
            else if (!world::sublevel_loaded(world, e)) {
                asset::Guid guid;
                const asset::AssetRecord* r = asset::parse_guid(sub->level, guid) ? registry.find(guid) : nullptr;
                char path[300];
                if (r && r->type == asset::AssetType::Level) {
                    registry.source_path(*r, path, sizeof path);
                    world::load_sublevel(world, e, path);
                }
            }
        }
    }

    // Level geometry is static; RigidBody objects are simulated.
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        if (const world::Entity e = world.entity_at_slot(slot); e.valid()) add_entity_bodies(world, e);
    }
    add_joints(world, {});
    agent_count_ = 0;
    const u64 nav_start = SDL_GetTicksNS();
    bool nav_reused = false;
    if (build_level_navmesh(world, *meshes_, nav_, &nav_hash_, &nav_reused)) {
        LSD_LOG_INFO("play", nav_reused ? "Navmesh unchanged: reused (%.0f ms)" : "Navmesh built (%.0f ms)", static_cast<f64>(SDL_GetTicksNS() - nav_start) * 1e-6);
    } else {
        nav_.clear();  // no navigation inputs now: nothing left from the last Play
    }
    brains_ = new Brain[kMaxBrains];
    brain_count_ = 0;
    noise_count_ = 0;
    Vec3 scale;

    // The player: the first Character, else a default walker at the Player Start or the camera.
    const reflect::TypeInfo& character_type = reflect::type_of<world::Character>();
    const reflect::TypeInfo& start_type = reflect::type_of<world::PlayerStart>();
    settings_ = world::Character{};
    character_ = {};
    crouched_ = swimming_ = false;
    wired_ = false;
    Vec3 rotation;
    if (world.count(character_type) > 0) {
        character_ = world.owner(character_type, 0);
        settings_ = *world.get<world::Character>(character_);
        live_character_ = settings_;  // live edits compare with this
        if (const auto* sprite = world.get<world::Sprite>(character_)) {
            idle_frame_ = sprite->frame;
            idle_count_ = sprite->frame_count;
        }
        decompose_trs(world.world_matrix(character_), feet_, rotation, scale);
        yaw_ = radians(rotation.z);
        pitch_ = 0.0f;
        if (load_asset(registry, settings_.mapping_context, context_)) {
            wired_ = true;
            if (!settings_.move_action[0] || !settings_.look_action[0] || !settings_.jump_action[0]) {
                LSD_LOG_WARN("play", "%s: pick its Move, Look and Jump actions in Details to control it", world.name(character_));
            }
        } else {
            // No input set up yet: the built-in controls, so a fresh Character can walk straight away.
            context_ = default_controls();
            std::snprintf(settings_.move_action, sizeof settings_.move_action, "%s", kDefaultMove);
            std::snprintf(settings_.look_action, sizeof settings_.look_action, "%s", kDefaultLook);
            std::snprintf(settings_.jump_action, sizeof settings_.jump_action, "%s", kDefaultJump);
            LSD_LOG_INFO("play", "%s: no Input Mapping Context, using the built-in WASD / mouse / Space controls", world.name(character_));
        }
    } else {
        context_ = default_controls();
        std::snprintf(settings_.move_action, sizeof settings_.move_action, "%s", kDefaultMove);
        std::snprintf(settings_.look_action, sizeof settings_.look_action, "%s", kDefaultLook);
        std::snprintf(settings_.jump_action, sizeof settings_.jump_action, "%s", kDefaultJump);
        if (world.count(start_type) > 0) {
            const world::Entity start = world.owner(start_type, 0);
            const auto* s = world.get<world::PlayerStart>(start);
            settings_.eye_height = s->eye_height;
            settings_.walk_speed = s->walk_speed;
            settings_.jump_speed = s->jump_speed;
            decompose_trs(world.world_matrix(start), feet_, rotation, scale);
            yaw_ = radians(rotation.z);
            pitch_ = 0.0f;
        } else {
            feet_ = eye - Vec3{0, 0, settings_.eye_height};
            yaw_ = yaw;
            pitch_ = pitch;
            LSD_LOG_INFO("play", "No Character or Player Start in the level: playing from the camera.");
        }
    }
    apply_rebinds();
    apply_input();
    input_stamp_ = input_files_stamp();
    key_held_[0] = key_pressed_[0] = 0;
    spawn_ = feet_;
    spawn_yaw_ = yaw_;
    physics::CharacterDesc desc;
    desc.feet = feet_;
    desc.radius = settings_.radius;
    desc.height = settings_.height;
    desc.max_slope_degrees = settings_.max_slope;
    desc.step_height = settings_.step_height;
    physics_->create_character(desc);
    eye_ = feet_ + Vec3{0, 0, settings_.eye_height};
    view_eye_ = eye_;
    view_forward_ = forward();

    script::ScriptHost host;
    host.user = this;
    host.set_view = [](void* user, world::Entity camera, f32 blend) { static_cast<PlaySession*>(user)->set_view(camera, blend); };
    host.camera_shake = [](void* user, f32 strength, f32 seconds, f32 frequency) { static_cast<PlaySession*>(user)->camera_shake(strength, seconds, frequency); };
    camera_view_override_ = 2;  // each Play starts with the Camera's own View
    bone_ride_valid_ = false;
    host.set_camera_view = [](void* user, u32 view) { static_cast<PlaySession*>(user)->camera_view_override_ = view; };
    host.camera_location = [](void* user) { return static_cast<PlaySession*>(user)->eye(); };
    host.camera_forward = [](void* user) { return static_cast<PlaySession*>(user)->view_forward(); };
    host.camera_view = [](void* user) -> u32 {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.camera_view_override_ < 2) return self.camera_view_override_;
        const world::Entity cam = self.world_ ? self.player_camera(*self.world_) : world::Entity{};
        const auto* pc = cam.valid() ? self.world_->get<world::Camera>(cam) : nullptr;
        return pc ? pc->view : (self.settings_.camera_distance > 0.0f ? 1u : 0u);
    };
    host.ik = [](void* user, world::Entity e, const char* bone, Vec3 target, f32 weight, bool aim, Vec3 forward) {
        auto& self = *static_cast<PlaySession*>(user);
        if (!self.animations_ || !self.world_) return;
        for (world::Entity m = e; m.valid(); m = {}) {  // the object, or its skeletal mesh child (a Character's)
            if (self.world_->get<world::SkeletalMesh>(m)) {
                self.animations_->add_ik(m, bone, target, weight, aim, forward);
                return;
            }
        }
        for (u32 i = 0; i < self.world_->count(reflect::type_of<world::SkeletalMesh>()); ++i) {
            const world::Entity m = self.world_->owner(reflect::type_of<world::SkeletalMesh>(), i);
            if (self.world_->parent(m) == e) self.animations_->add_ik(m, bone, target, weight, aim, forward);
        }
    };
    if (animations_) {
        animations_->set_ground_probe([](void* user, Vec3 from, f32 distance, Vec3& hit) {
            physics::RayHit ray;
            auto& self = *static_cast<PlaySession*>(user);
            if (!self.physics_ || !self.physics_->raycast(from, Vec3{0, 0, -1}, distance, ray)) return false;
            hit = ray.point;
            return true;
        }, this);
    }
    host.socket = [](void* user, world::Entity e, const char* name, Vec3& position, Vec3& forward) {
        auto& self = *static_cast<PlaySession*>(user);
        if (!self.animations_ || !self.skinned_) return false;
        Mat4 m;
        bool ok = self.animations_->socket(*self.world_, *self.skinned_, e, name, m);
        for (u32 i = 0; !ok && i < self.world_->count(reflect::type_of<world::SkeletalMesh>()); ++i) {  // a Character's mesh child
            const world::Entity child = self.world_->owner(reflect::type_of<world::SkeletalMesh>(), i);
            if (self.world_->parent(child) == e) ok = self.animations_->socket(*self.world_, *self.skinned_, child, name, m);
        }
        if (!ok) return false;
        position = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        forward = normalize(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
        return true;
    };
    host.timeline = [](void* user, int what, const char* name, bool loop) {
        auto& self = *static_cast<PlaySession*>(user);
        if (what == 0) return self.play_timeline(name, loop);
        self.stop_timeline(name);
        return true;
    };
    host.music = [](void* user, int what, const char* name, f32 volume, f32 fade) {
        SoundPlayer* sounds = static_cast<PlaySession*>(user)->sounds_;
        if (!sounds) return;
        if (what == 0) sounds->play_music(name, fade);
        else if (what == 1) sounds->stop_music(fade);
        else sounds->set_music_layer(name, volume, fade);
    };
    host.input = [](void* user, const char* action) {
        const auto& self = *static_cast<const PlaySession*>(user);
        char guid[33];
        if (!action_guid(*self.registry_, action, guid)) return Vec3{};
        const input::Value v = self.input_.value(guid);
        return Vec3{v.x, v.y, 0};
    };
    host.pressed = [](void* user, const char* action) {
        const auto& self = *static_cast<const PlaySession*>(user);
        char guid[33];
        return action_guid(*self.registry_, action, guid) && self.input_.fired(guid);
    };
    host.draw_debug = [](void*, int shape, Vec3 a, Vec3 b, f32 radius, const char* text, Vec3 color, f32 seconds) {
        if (shape == 0) debug_draw::line(a, b, color, seconds);
        else if (shape == 1) debug_draw::sphere(a, radius, color, seconds);
        else if (shape == 2) debug_draw::box(a, b, color, seconds);
        else debug_draw::text(a, text, color, seconds);
    };
    host.rebind = [](void* user, const char* action, const char* from, const char* to) { return static_cast<PlaySession*>(user)->rebind(action, from, to); };
    host.bindings = [](void* user, const char* action, const char** out, u32 capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        char guid[33];
        u32 n = 0;
        if (!action_guid(*self.registry_, action, guid)) return n;
        for (const input::KeyMapping& m : self.context_.mappings) {
            if (n < capacity && std::strcmp(m.action, guid) == 0) out[n++] = m.key;
        }
        return n;
    };
    host.key_pressed = [](void* user) -> const char* {
        const auto& self = *static_cast<const PlaySession*>(user);
        return self.key_pressed_[0] ? self.key_pressed_ : nullptr;
    };
    host.spawn = [](void* user, const char* name, Vec3 position) { return static_cast<PlaySession*>(user)->spawn(name, position); };
    host.raycast = [](void* user, Vec3 origin, Vec3 direction, f32 distance, world::Entity& hit, Vec3& point, Vec3& normal) {
        const auto& self = *static_cast<const PlaySession*>(user);
        physics::RayHit ray;
        if (!self.physics_->raycast(origin, direction, distance, ray)) return false;
        hit = self.body_entity(ray.body);
        point = ray.point;
        normal = ray.normal;
        return true;
    };
    host.sweep = [](void* user, int shape, Vec3 size, Vec3 from, Vec3 to, world::Entity& hit, Vec3& point, Vec3& normal) {
        const auto& self = *static_cast<const PlaySession*>(user);
        physics::RayHit result;
        if (!self.physics_->sweep(query_shape(shape, size), from, to, result)) return false;
        hit = self.body_entity(result.body);
        point = result.point;
        normal = result.normal;
        return true;
    };
    host.overlap = [](void* user, int shape, Vec3 size, Vec3 at, world::Entity* out, u32 capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        u32 bodies[64];
        const u32 n = self.physics_->overlap(query_shape(shape, size), at, bodies, capacity < 64 ? capacity : 64);
        u32 written = 0;
        for (u32 i = 0; i < n; ++i) {
            const world::Entity e = self.body_entity(bodies[i]);
            if (e.valid()) out[written++] = e;
        }
        return written;
    };
    host.open_level = [](void* user, const char* name) {
        auto& self = *static_cast<PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::Level, name);
        if (!r) {
            LSD_LOG_WARN("play", "open_level: no level named '%s'", name);
            return false;
        }
        // Multiplayer travels together (Unreal's seamless ServerTravel, simply): the server takes
        // everyone along; a client cannot leave on its own.
        if (self.net_.mode() == net::Mode::Client) {
            LSD_LOG_WARN("play", "open_level: in multiplayer only the server changes the level");
            return false;
        }
        if (self.net_.mode() == net::Mode::Server) self.net_.send_event(net::kToEveryone, "__travel", ("[" + json_string(r->name) + "]").c_str());
        self.registry_->source_path(*r, self.level_request_, sizeof self.level_request_);
        return true;
    };
    host.player = [](void* user) { return static_cast<const PlaySession*>(user)->character_; };
    host.table_path = [](void* user, const char* name, char* out, usize capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::Table, name);
        if (r) self.registry_->source_path(*r, out, capacity);
        return r != nullptr;
    };
    host.play_sound = [](void* user, const char* name, bool spatial, Vec3 position) {
        auto& self = *static_cast<PlaySession*>(user);
        return self.sounds_ ? self.sounds_->play(name, spatial, position) : 0u;
    };
    host.push_mix = [](void* user, const char* name) {
        if (auto* sounds = static_cast<PlaySession*>(user)->sounds_) sounds->push_mix(name);
    };
    host.pop_mix = [](void* user, const char* name) {
        if (auto* sounds = static_cast<PlaySession*>(user)->sounds_) sounds->pop_mix(name);
    };
    host.set_audio_param = [](void* user, const char* name, f32 value) {
        if (auto* sounds = static_cast<PlaySession*>(user)->sounds_) sounds->set_param(name, value);
    };
    host.play_montage = [](void* user, world::Entity e, const char* animation, f32 rate, const char* from_bone, f32 blend, bool root_motion) {
        auto& self = *static_cast<PlaySession*>(user);
        return self.animations_ && self.skinned_ ? self.animations_->play_montage(*self.world_, *self.skinned_, e, animation, rate, from_bone, blend, root_motion) : 0.0f;
    };
    host.stop_montage = [](void* user, world::Entity e, f32 blend) {
        if (auto* a = static_cast<PlaySession*>(user)->animations_) a->stop_montage(e, blend);
    };
    host.montage_playing = [](void* user, world::Entity e) {
        const auto* a = static_cast<PlaySession*>(user)->animations_;
        return a && a->montage_playing(e);
    };
    host.set_anim_param = [](void* user, world::Entity e, const char* name, f32 value) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.animations_) self.animations_->set_param(*self.world_, e, name, value);
    };
    host.sublevel = [](void* user, world::Entity e, bool load) {
        auto& self = *static_cast<PlaySession*>(user);
        return self.world_ && self.stream_sublevel(*self.world_, e, load);
    };
    host.ride = [](void* user, int what, world::Entity e) {
        auto& self = *static_cast<PlaySession*>(user);
        return self.world_ ? self.ride_command(*self.world_, what, e) : 0u;
    };
    host.net_mode = [](void* user) {
        const net::Mode mode = static_cast<PlaySession*>(user)->net_.mode();
        return mode == net::Mode::Server ? 1 : mode == net::Mode::Client ? 2 : 0;
    };
    host.net_player = [](void* user) {
        const auto& net = static_cast<PlaySession*>(user)->net_;
        return net.active() ? net.player_id() : 0u;
    };
    host.net_send = [](void* user, i32 target, const char* name, const char* json) { static_cast<PlaySession*>(user)->net_.send_event(target, name, json); };
    host.net_chat = [](void* user, const char* channel, const char* text) { return static_cast<PlaySession*>(user)->send_chat(channel, text); };
    host.net_set_group = [](void* user, u32 player, u32 team, u32 squad) { static_cast<PlaySession*>(user)->net_.set_player_group(player, team, squad); };
    host.net_group = [](void* user, u32 player, u32& team, u32& squad) {
        const auto& net = static_cast<PlaySession*>(user)->net_;
        team = net.player_team(player);
        squad = net.player_squad(player);
    };
    host.net_seen_position = [](void* user, u32 entity, u32 player, Vec3& out) {
        auto& self = *static_cast<PlaySession*>(user);
        world::Entity e;
        e.value = entity;
        return self.world_ && self.net_.seen_position(*self.world_, e, player, out);
    };
    host.net_session = [](void* user, int what, const char* arg, char* why, usize capacity) {
        auto& self = *static_cast<PlaySession*>(user);
        NetPlay& play = self.net_play_;
        if (!play.allowed) {
            std::snprintf(why, capacity, "turn on Project Settings > Multiplayer");
            return false;
        }
        char path[300];
        std::snprintf(path, sizeof path, "%s", self.current_level_);
        if (what != 1 && arg[0]) {
            const asset::AssetRecord* r = self.find_asset(asset::AssetType::Level, arg);
            if (!r) {
                std::snprintf(why, capacity, "no level named '%s'", arg);
                return false;
            }
            self.registry_->source_path(*r, path, sizeof path);
        }
        if (what == 1) {
            char address[64];
            if (!net::parse_join_code(arg, play.settings.port, address, sizeof address)) {
                std::snprintf(why, capacity, "'%s' is not a join code (10 letters and digits) or an address", arg);
                return false;
            }
            std::snprintf(play.address, sizeof play.address, "%s", address);
            LSD_LOG_INFO("net", "Joining %s", address);
        }
        play.enabled = what != 2;
        play.host = what == 0;
        play.dedicated = false;
        std::snprintf(self.level_request_, sizeof self.level_request_, "%s", path);
        return true;
    };
    host.net_join_codes = [](void* user, char* out, usize capacity) { static_cast<PlaySession*>(user)->join_codes(out, capacity, true); };
    host.editor_workspace = [](void* user) -> const char* { return static_cast<PlaySession*>(user)->workspace_; };
    host.net_find_games = [](void* user, f32 seconds, char* out, usize capacity) {
        const auto& self = *static_cast<PlaySession*>(user);
        net::LanGame games[32];
        const u32 n = net::NetSession::find_lan_games(self.net_play_.settings.port, std::clamp(seconds, 0.05f, 5.0f), games, 32);
        usize used = 0;
        for (u32 k = 0; k < n && used < capacity; ++k) {
            const int w = std::snprintf(out + used, capacity - used, "%s|%s|%u|%u\n", games[k].address, games[k].name, games[k].players, games[k].max_players);
            if (w > 0) used += static_cast<usize>(w);
        }
        return n;
    };
    host.set_class_volume = [](void* user, const char* name, f32 volume) {
        if (auto* sounds = static_cast<PlaySession*>(user)->sounds_) sounds->set_class_volume(name, volume);
    };
    host.move_to = [](void* user, world::Entity e, Vec3 target) {
        auto& self = *static_cast<PlaySession*>(user);
        const i32 agent = self.agent_of(e);
        return agent >= 0 && self.nav_.move_agent(agent, target);
    };
    host.stop_move = [](void* user, world::Entity e) {
        auto& self = *static_cast<PlaySession*>(user);
        self.nav_.stop_agent(self.agent_of(e));
    };
    host.is_moving = [](void* user, world::Entity e) {
        auto& self = *static_cast<PlaySession*>(user);
        return self.nav_.agent_moving(self.agent_of(e));
    };
    host.find_path = [](void* user, Vec3 from, Vec3 to, Vec3* out, u32 capacity) { return static_cast<PlaySession*>(user)->nav_.find_path(from, to, out, capacity); };
    host.ui_open = [](void* user, const char* name) -> i32 {
        auto& self = *static_cast<PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::UiDocument, name);
        if (!r || !self.ui_) {
            LSD_LOG_WARN("ui", "ui_open: no UI document named '%s'", name);
            return -1;
        }
        char path[300];
        self.registry_->source_path(*r, path, sizeof path);
        return self.ui_->open(path);
    };
    host.ui_close = [](void* user, i32 doc) {
        if (auto* ui = static_cast<PlaySession*>(user)->ui_) ui->close(doc);
    };
    host.ui_set = [](void* user, i32 doc, const char* id, const char* attribute, const char* value) {
        auto* ui = static_cast<PlaySession*>(user)->ui_;
        return ui && (attribute ? ui->set_attribute(doc, id, attribute, value) : ui->set_text(doc, id, value));
    };
    host.ui_data = [](void* user, const char* name, int type, f64 number, const char* text) {
        auto* ui = static_cast<PlaySession*>(user)->ui_;
        if (!ui) return;
        if (type == 0) ui->set_data(name, number);
        else if (type == 2) ui->set_data(name, number != 0.0);
        else ui->set_data(name, text);
    };
    host.ui_value = [](void* user, i32 doc, const char* id, char* out, usize capacity) {
        auto* ui = static_cast<PlaySession*>(user)->ui_;
        return ui && ui->get_value(doc, id, out, capacity);
    };
    host.ui_show_cursor = [](void* user, bool on) { static_cast<PlaySession*>(user)->ui_cursor_ = on; };
    host.ui_follow = [](void* user, i32 doc, const char* id, world::Entity e, f32 height) {
        auto& self = *static_cast<PlaySession*>(user);
        for (u32 i = 0; i < self.ui_follower_count_; ++i) {  // replace (or drop) an existing one
            UiFollower& f = self.ui_followers_[i];
            if (f.doc != doc || std::strcmp(f.id, id) != 0) continue;
            if (e.valid()) {
                f.entity = e;
                f.height = height;
            } else {
                self.ui_followers_[i] = self.ui_followers_[--self.ui_follower_count_];
            }
            return;
        }
        if (!e.valid() || self.ui_follower_count_ == sizeof self.ui_followers_ / sizeof self.ui_followers_[0]) return;
        UiFollower& f = self.ui_followers_[self.ui_follower_count_++];
        f.doc = doc;
        std::snprintf(f.id, sizeof f.id, "%s", id);
        f.entity = e;
        f.height = height;
    };
    host.settings = [](void* user, const reflect::TypeInfo** type) -> void* {
        auto& self = *static_cast<PlaySession*>(user);
        *type = &reflect::type_of<GameUserSettings>();
        return self.user_settings_;
    };
    host.settings_changed = [](void* user) { static_cast<PlaySession*>(user)->settings_changed_ = true; };
    host.text = [](void* user, const char* key) {
        const auto* loc = static_cast<PlaySession*>(user)->localization_;
        return loc ? loc->text(key) : key;
    };
    host.language = [](void* user) {
        const auto* loc = static_cast<PlaySession*>(user)->localization_;
        return loc ? loc->language() : "en";
    };
    host.set_language = [](void* user, const char* code) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.localization_ && !self.localization_->set_language(code)) LSD_LOG_WARN("ui", "The string table has no language '%s'", code);
        if (self.user_settings_) {
            std::snprintf(self.user_settings_->language, sizeof self.user_settings_->language, "%s", code);
            self.settings_changed_ = true;  // saved with the other settings
        }
        if (self.ui_) self.ui_->reload_all();  // documents translate as they load
    };
    host.languages = [](void* user, const char** out, u32 capacity) {
        const auto* loc = static_cast<PlaySession*>(user)->localization_;
        u32 n = 0;
        for (; loc && n < loc->language_count() && n < capacity; ++n) out[n] = loc->language_code(n);
        return n;
    };
    host.bb_set = [](void* user, world::Entity e, const char* key, const f32* value, bool vector) {
        if (Brain* b = static_cast<PlaySession*>(user)->brain_of(e)) {
            if (vector) b->board.set(key, Vec3{value[0], value[1], value[2]});
            else b->board.set(key, value[0]);
        }
    };
    host.bb_get = [](void* user, world::Entity e, const char* key, f32* value) {
        const Brain* b = static_cast<PlaySession*>(user)->brain_of(e);
        const ai::Blackboard::Entry* entry = b ? b->board.find(key) : nullptr;
        if (!entry) return 0;
        for (int i = 0; i < 3; ++i) value[i] = entry->value[i];
        return entry->vector ? 3 : 1;
    };
    host.bb_clear = [](void* user, world::Entity e, const char* key) {
        if (Brain* b = static_cast<PlaySession*>(user)->brain_of(e)) b->board.clear(key);
    };
    host.report_noise = [](void* user, Vec3 at, f32 loudness) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.noise_count_ < sizeof self.noises_ / sizeof self.noises_[0]) self.noises_[self.noise_count_++] = {at, loudness};
    };
    host.stop_sound = [](void* user, u32 voice) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.sounds_) self.sounds_->audio()->stop(voice);
    };
    // Sound Emitters set to Auto Play start with the game.
    if (sounds_) {
        sounds_->stop_all_and_forget();  // classes are read fresh: edits since the last Play count
        asset::Guid base;
        if (const asset::AssetRecord* r = base_mix_ && asset::parse_guid(base_mix_, base) ? registry.find(base) : nullptr) sounds_->push_mix(r->name);
        const reflect::TypeInfo& emitter_type = reflect::type_of<world::SoundEmitter>();
        for (u32 i = 0; i < world.count(emitter_type) && emitter_count_ < sizeof emitters_ / sizeof emitters_[0]; ++i) {
            const auto* emitter = static_cast<const world::SoundEmitter*>(world.at(emitter_type, i));
            const world::Entity e = world.owner(emitter_type, i);
            if (!emitter->auto_play || !emitter->sound[0]) continue;
            const Mat4 m = world.world_matrix(e);
            emitters_[emitter_count_++] = {e, sounds_->play(emitter->sound, true, {m.at(0, 3), m.at(1, 3), m.at(2, 3)})};
        }
    }
    host.data_path = [](void* user, const char* data_type, const char* name, char* out, usize capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::Data, name);
        const bool ok = r && std::strcmp(r->data_type, data_type) == 0;
        if (ok) self.registry_->source_path(*r, out, capacity);
        return ok;
    };
    host.curve_path = [](void* user, const char* name, char* out, usize capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::Data, name);
        if (r && std::strcmp(r->data_type, "Curve") == 0) self.registry_->source_path(*r, out, capacity);
        return r && std::strcmp(r->data_type, "Curve") == 0;
    };
    scripts_.start(world, host, script_path, const_cast<asset::AssetRegistry*>(&registry));
    LSD_LOG_INFO("play", "Play started in %.0f ms", static_cast<f64>(SDL_GetTicksNS() - start_ns) * 1e-6);
}

const asset::AssetRecord* PlaySession::find_asset(asset::AssetType type, const char* name) const {
    usize hits[16];
    const usize n = registry_->search(name, type, nullptr, hits, 16);
    for (usize i = 0; i < n; ++i) {
        if (std::strcmp(registry_->get(hits[i]).name, name) == 0) return &registry_->get(hits[i]);
    }
    return nullptr;
}

u32 PlaySession::add_body(world::Entity e, const physics::BodyDesc& desc) {
    const u32 body = physics_->add_body(desc);
    if (body < kMaxBodies) body_entities_[body] = e;
    return body;
}

world::Entity PlaySession::body_entity(u32 body) const {
    if (body == physics::kCharacterBody) return character_;
    return body < kMaxBodies ? body_entities_[body] : world::Entity{};
}

// Meshes with collision and Blocking Volumes become static bodies, RigidBody objects dynamic ones.
void PlaySession::add_entity_bodies(const world::World& world, world::Entity e) {
    if (world.get<world::Character>(e)) return;  // the player is the character controller, not a body
    Vec3 scale, centre;
    const auto* mesh = world.get<world::StaticMesh>(e);
    const CachedMesh* cached = mesh ? meshes_->get(mesh->mesh) : nullptr;
    if (const auto* follower = world.get<world::SplineFollower>(e)) {  // moved along its spline; kinematic when solid
        if (follower_count_ == sizeof followers_ / sizeof followers_[0]) return;
        Follower& f = followers_[follower_count_++];
        f = Follower{e, {}, 0.0f, 1.0f, physics::kWorldBody, {1, 1, 1}, {}};
        for (u32 slot = 0; slot < world.slot_count() && !f.spline.valid(); ++slot) {
            const world::Entity candidate = world.entity_at_slot(slot);
            if (candidate.valid() && world.get<world::Spline>(candidate) && std::strcmp(world.name(candidate), follower->spline) == 0) f.spline = candidate;
        }
        if (!f.spline.valid()) LSD_LOG_WARN("play", "%s: Spline Follower: no object named '%s' with a Spline", world.name(e), follower->spline);
        if (cached && mesh->collision) {
            f.body = add_body(e, fitted_body(world.world_matrix(e), cached->bounds_min, cached->bounds_max, 0, physics::Motion::Kinematic, f.scale, f.centre));
        }
        return;
    }
    // The mesh's points (and triangles) around the bounds centre, scaled: for Mesh and Convex shapes.
    std::vector<Vec3> points;
    const MeshCache::Triangles* triangles = nullptr;
    auto use_mesh_shape = [&](physics::BodyDesc& d, physics::Shape shape) {
        triangles = mesh ? meshes_->triangles(mesh->mesh) : nullptr;
        if (!triangles) return;
        points.clear();
        for (const Vec3& p : triangles->points) points.push_back(mul(p - centre, scale));
        d.shape = shape;
        d.points = points.data();
        d.point_count = static_cast<u32>(points.size());
        d.indices = triangles->indices.data();
        d.index_count = static_cast<u32>(triangles->indices.size());
    };
    // A Physical Material asset overrides friction and bounciness.
    // shortcut: read from disk per body at Play start; cache by GUID if levels get thousands of them.
    auto apply_material = [&](physics::BodyDesc& d, const char* guid) {
        world::PhysicalMaterial material;
        if (!guid[0] || !load_asset(*registry_, guid, material)) return;
        d.friction = material.friction;
        d.restitution = material.bounciness;
    };
    if (const auto* rigid = world.get<world::RigidBody>(e)) {
        if (simulated_count_ == sizeof simulated_ / sizeof simulated_[0]) return;
        // A client shows the server's simulation: its replicated bodies follow their Transform.
        const bool remote = net_.mode() == net::Mode::Client && world.get<world::NetReplicated>(e);
        const physics::Motion motion = rigid->kinematic || remote ? physics::Motion::Kinematic : physics::Motion::Dynamic;
        physics::BodyDesc d = fitted_body(world.world_matrix(e), cached ? cached->bounds_min : kUnitLo, cached ? cached->bounds_max : kUnitHi,
                                          rigid->shape == 3 ? 0 : rigid->shape, motion, scale, centre);
        if (rigid->shape == 3) use_mesh_shape(d, physics::Shape::Convex);
        d.mass = rigid->mass;
        d.friction = rigid->friction;
        d.restitution = rigid->bounciness;
        d.debris = rigid->collision_preset == 1;
        d.plane_2d = rigid->lock_2d;
        apply_material(d, rigid->physical_material);
        const u32 body = add_body(e, d);
        simulated_[simulated_count_++] = {e, body, scale, centre, rigid->kinematic || remote, rigid->mass, rigid->friction, rigid->bounciness};
        return;
    }
    if (const auto* vehicle = world.get<world::Vehicle>(e); vehicle && vehicle->model == 1 && vehicle->kind == 0) {  // a body on wheels
        add_physics_vehicle(world, e, cached, *vehicle);
        return;
    }
    if (world.get<world::Mover>(e) || world.get<world::Vehicle>(e)) {  // moved by Play: kinematic, carries what stands on it
        add_moving(world, e, cached, !mesh || mesh->collision);
        return;
    }
    if (cached && mesh->collision) {
        physics::BodyDesc d = fitted_body(world.world_matrix(e), cached->bounds_min, cached->bounds_max, 0, physics::Motion::Static, scale, centre);
        if (mesh->collision_shape == 1) use_mesh_shape(d, physics::Shape::Mesh);
        apply_material(d, mesh->physical_material);
        add_body(e, d);
    }
    if (const auto* ism = world.get<world::InstancedStaticMesh>(e); ism && ism->collision && ism->instances.count) {
        // Per-copy collision (foliage trees and rocks): a static box per copy, fitted to the mesh's bounds.
        // shortcut: a body per copy (Jolt handles thousands of static boxes); one compound body per object if levels get dense
        if (const CachedMesh* copy_mesh = meshes_->get(ism->mesh)) {
            const Mat4 m = world.world_matrix(e);
            for (u32 k = 0; k < ism->instances.count; ++k) {
                const world::MeshInstance& in = ism->instances[k];
                Vec3 copy_scale, copy_centre;
                add_body(e, fitted_body(m * trs(in.position, in.rotation, in.scale), copy_mesh->bounds_min, copy_mesh->bounds_max, 0, physics::Motion::Static, copy_scale, copy_centre));
            }
        }
    }
    if (const auto* sprite = world.get<world::Sprite>(e); sprite && sprite->collision && !cached) {
        add_body(e, fitted_body(world.world_matrix(e), kUnitLo, kUnitHi, 0, physics::Motion::Static, scale, centre));  // the sprite's quad, 1 m deep
    }
    if (const auto* map = world.get<world::TileMap>(e); map && map->collision) {  // a box per run of solid tiles in a row
        static TileRun runs[4096];
        const u32 count = tile_runs(*map, runs, 4096);
        for (u32 k = 0; k < count; ++k) {
            Vec3 run_scale, run_centre;
            add_body(e, fitted_body(world.world_matrix(e), runs[k].lo, runs[k].hi, 0, physics::Motion::Static, run_scale, run_centre));
        }
    }
    if (const auto* blocking = world.get<world::BlockingVolume>(e); blocking && blocking->enabled) {
        add_body(e, fitted_body(world.world_matrix(e), kUnitLo, kUnitHi, 0, physics::Motion::Static, scale, centre));
    }
}

// A prefab, mesh asset, particle system or built-in shape by name, placed at `position` (world objects only).
world::Entity PlaySession::create_spawn(world::World& world, const char* name, Vec3 position) {
    const asset::AssetRecord* prefab = find_asset(asset::AssetType::Prefab, name);
    const asset::AssetRecord* mesh_asset = prefab ? nullptr : find_asset(asset::AssetType::Mesh, name);
    const asset::AssetRecord* effect = prefab || mesh_asset ? nullptr : find_asset(asset::AssetType::Data, name);
    if (effect && std::strcmp(effect->data_type, "ParticleSystem") != 0) effect = nullptr;
    char key[40];
    if (mesh_asset) asset::to_string(mesh_asset->guid, key);
    else std::snprintf(key, sizeof key, "%s", name);  // built-in shapes: Cube, Sphere...
    if (!prefab && !effect && !meshes_->get(key)) return {};

    char unique[64];
    world.unique_name(name, unique, sizeof unique);
    const world::Entity e = world.create(unique);
    world.get<world::Transform>(e)->position = position;
    if (effect) {  // an explosion or a puff of dust at a point (Unreal's Spawn System at Location)
        auto* fx = world.add<world::ParticleEffect>(e);
        asset::to_string(effect->guid, fx->system);
        fx->auto_destroy = true;  // gone once it has played out (a looping one plays until destroyed)
    } else if (prefab) {
        asset::to_string(prefab->guid, world.add<world::PrefabInstance>(e)->prefab);
        char path[300];
        registry_->source_path(*prefab, path, sizeof path);
        std::string& text = prefab_text_[path];
        if (text.empty()) {  // shortcut: an edit to the prefab during Play shows on the next Play, read it per spawn if that matters
            std::ifstream file(path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        world::update_instance(world, e, text.data(), text.size(), true);
    } else {
        std::snprintf(world.add<world::StaticMesh>(e)->mesh, sizeof(world::StaticMesh::mesh), "%s", key);
    }
    return e;
}

void PlaySession::join_codes(char* out, usize capacity, bool map_port) {
    const u16 port = net_play_.settings.port;
    if (map_port && net_.mode() == net::Mode::Server && upnp_.state() == net::PortMapper::State::Off) upnp_.start(port);
    static const char* const states[] = {"off", "working", "open", "failed"};
    char internet[16] = "";
    if (upnp_.state() == net::PortMapper::State::Open) net::make_join_code(upnp_.external_address(), port, internet, sizeof internet);
    int used = std::snprintf(out, capacity, "upnp|%s|%s|%s\n", states[static_cast<int>(upnp_.state())], upnp_.message(), internet);
    char locals[8][16];
    const u32 n = net::local_addresses(locals, 8);  // shortcut: asks the OS each call; cache it if the panel's refresh shows up in stat dump
    for (u32 k = 0; k < n && used > 0 && static_cast<usize>(used) < capacity; ++k) {
        char code[16];
        net::make_join_code(locals[k], port, code, sizeof code);
        used += std::snprintf(out + used, capacity - static_cast<usize>(used), "lan|%s|%s\n", locals[k], code);
    }
}

bool PlaySession::take_level_request(char* path, usize capacity) {
    if (!level_request_[0]) return false;
    std::snprintf(path, capacity, "%s", level_request_);
    if (std::strcmp(level_request_, "*") != 0) {  // the level's name is its file's
        std::snprintf(current_level_, sizeof current_level_, "%s", level_request_);
        const char* slash = std::strrchr(level_request_, '/');
        const char* back = std::strrchr(level_request_, '\\');
        const char* file = slash && back ? (slash > back ? slash : back) + 1 : slash ? slash + 1 : back ? back + 1 : level_request_;
        std::snprintf(level_name_, sizeof level_name_, "%s", file);
        if (char* dot = std::strrchr(level_name_, '.')) *dot = '\0';
    }
    level_request_[0] = '\0';
    return true;
}

world::Entity PlaySession::spawn(const char* name, Vec3 position) {
    world::World& world = *world_;
    const world::Entity root = create_spawn(world, name, position);
    if (!root.valid()) {
        LSD_LOG_WARN("play", "spawn: no prefab, mesh or shape named '%s'", name);
        return {};
    }
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const world::Entity e = world.entity_at_slot(slot);
        bool inside = false;
        for (world::Entity p = e; p.valid() && !inside; p = world.parent(p)) inside = p == root;
        if (inside) add_entity_bodies(world, e);
    }
    add_joints(world, root);
    scripts_.attach(root);
    return root;
}

// World Settings > Game Mode: spawn the player prefab (when the level has no Character) and the game script.
u32 PlaySession::body_of(world::Entity e) const {
    for (u32 b = 0; b < physics_->body_count() && b < kMaxBodies; ++b) {
        if (body_entities_[b] == e) return b;
    }
    return physics::kWorldBody;
}

void PlaySession::add_joints(const world::World& world, world::Entity root) {
    const reflect::TypeInfo& type = reflect::type_of<world::PhysicsJoint>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const world::Entity e = world.owner(type, i);
        bool inside = !root.valid();
        for (world::Entity p = e; p.valid() && !inside; p = world.parent(p)) inside = p == root;
        if (!inside) continue;
        const auto* joint = static_cast<const world::PhysicsJoint*>(world.at(type, i));
        const u32 body = body_of(e);
        if (body == physics::kWorldBody) {
            LSD_LOG_WARN("play", "%s: a Physics Joint needs a RigidBody (or collision) on the same object", world.name(e));
            continue;
        }
        physics::JointDesc d;
        d.type = static_cast<physics::JointType>(joint->type);
        d.body_a = body;
        if (joint->other[0]) {
            world::Entity other;
            for (u32 slot = 0; slot < world.slot_count() && !other.valid(); ++slot) {
                const world::Entity candidate = world.entity_at_slot(slot);
                if (candidate.valid() && std::strcmp(world.name(candidate), joint->other) == 0) other = candidate;
            }
            if (!other.valid()) LSD_LOG_WARN("play", "%s: Physics Joint other '%s' not found: held to the world", world.name(e), joint->other);
            d.body_b = other.valid() ? body_of(other) : physics::kWorldBody;
        }
        const Mat4 m = world.world_matrix(e);
        d.pivot = transform_point(m, joint->pivot);
        d.axis = transform_vector(m, joint->axis);
        d.limited = joint->limited;
        d.limit_min = joint->limit_min;
        d.limit_max = joint->limit_max;
        d.motor_speed = joint->motor_speed;
        d.motor_strength = joint->motor_strength;
        d.spring_frequency = joint->spring_frequency;
        d.spring_damping = joint->spring_damping;
        d.cone_angle = joint->cone_angle;
        d.break_force = joint->break_force;
        const u32 index = physics_->add_joint(d);
        if (index < kMaxJoints) joint_owners_[index] = e;
    }
}

void PlaySession::start_game_mode(world::World& world) {
    if (!world.settings().game_mode[0]) return;
    world::GameMode mode;
    if (!load_asset(*registry_, world.settings().game_mode, mode)) {
        LSD_LOG_WARN("play", "World Settings: the Game Mode asset is missing");
        return;
    }
    const reflect::TypeInfo& start_type = reflect::type_of<world::PlayerStart>();
    if (mode.player_prefab[0] && world.count(reflect::type_of<world::Character>()) == 0) {
        asset::Guid guid;
        const asset::AssetRecord* r = asset::parse_guid(mode.player_prefab, guid) ? registry_->find(guid) : nullptr;
        Vec3 at, rotation, scale;
        if (world.count(start_type) > 0) decompose_trs(world.world_matrix(world.owner(start_type, 0)), at, rotation, scale);
        const world::Entity player = r ? create_spawn(world, r->name, at) : world::Entity{};
        if (player.valid()) world.get<world::Transform>(player)->rotation.z = rotation.z;
        if (!player.valid() || !world.get<world::Character>(player)) LSD_LOG_WARN("play", "Game Mode: the player prefab is missing or has no Character component");
    }
    if (mode.script[0]) std::snprintf(world.add<world::Script>(world.create("GameMode"))->script, sizeof(world::Script::script), "%s", mode.script);
    if (mode.player_controller[0]) {  // Unreal's Player Controller: lives as long as the level, whatever body the player has
        std::snprintf(world.add<world::Script>(world.create("PlayerController"))->script, sizeof(world::Script::script), "%s", mode.player_controller);
    }
    asset::Guid hud;
    const asset::AssetRecord* r = mode.hud[0] && asset::parse_guid(mode.hud, hud) ? registry_->find(hud) : nullptr;
    if (r && ui_) {
        char path[300];
        registry_->source_path(*r, path, sizeof path);
        if (ui_->open(path) < 0) LSD_LOG_WARN("play", "Game Mode: could not open the HUD %s", path);
    }
    ui_cursor_ = mode.show_cursor;
}

void PlaySession::place_ui_followers(const world::World& world, const Mat4& view_proj, const f32 viewport[4]) {
    if (!ui_) return;
    for (u32 i = 0; i < ui_follower_count_;) {
        const UiFollower& f = ui_followers_[i];
        if (!world.alive(f.entity) || !ui_->is_open(f.doc)) {
            ui_followers_[i] = ui_followers_[--ui_follower_count_];
            continue;
        }
        const Mat4 m = world.world_matrix(f.entity);
        const Vec3 p{m.at(0, 3), m.at(1, 3), m.at(2, 3) + f.height};
        const f32 x = view_proj.at(0, 0) * p.x + view_proj.at(0, 1) * p.y + view_proj.at(0, 2) * p.z + view_proj.at(0, 3);
        const f32 y = view_proj.at(1, 0) * p.x + view_proj.at(1, 1) * p.y + view_proj.at(1, 2) * p.z + view_proj.at(1, 3);
        const f32 w = view_proj.at(3, 0) * p.x + view_proj.at(3, 1) * p.y + view_proj.at(3, 2) * p.z + view_proj.at(3, 3);
        if (w <= 0.05f) {
            ui_->set_property(f.doc, f.id, "visibility", "hidden");  // behind the camera
        } else {
            char left[32], top[32];
            std::snprintf(left, sizeof left, "%.1fpx", static_cast<f64>((x / w * 0.5f + 0.5f) * viewport[2]));
            std::snprintf(top, sizeof top, "%.1fpx", static_cast<f64>((0.5f - y / w * 0.5f) * viewport[3]));
            ui_->set_property(f.doc, f.id, "left", left);
            ui_->set_property(f.doc, f.id, "top", top);
            ui_->set_property(f.doc, f.id, "visibility", "visible");
        }
        ++i;
    }
}

void PlaySession::set_ui(ui::UiContext* ui) {
    ui_ = ui;
    if (!ui_) return;
    ui_->set_actions([](void* user, ui::UiContext&, ui::DocumentId, const char* action, const char* element_id) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.scripts_.broadcast(action, element_id) == 0) LSD_LOG_WARN("ui", "No script has a function '%s' (from #%s)", action, element_id);
    }, this);
}

void PlaySession::update_followers(world::World& world, f32 dt) {
    for (u32 i = 0; i < follower_count_; ++i) {
        Follower& f = followers_[i];
        const auto* follower = world.get<world::SplineFollower>(f.entity);
        const auto* spline = world.alive(f.spline) ? world.get<world::Spline>(f.spline) : nullptr;
        if (!follower || !spline || spline->points.count < 2) continue;
        Vec3 points[32], tangents[32];
        bool closed = false;
        world::spline_world_curve(world, f.spline, points, tangents, closed);
        const f32 total = world::spline_length(points, spline->points.count, spline->closed, tangents);
        f.distance += follower->speed * f.direction * dt;
        if (follower->mode == 1) {  // Loop
            f.distance = total > 0.0f ? std::fmod(f.distance, total) : 0.0f;
            if (f.distance < 0.0f) f.distance += total;
        } else if (follower->mode == 2) {  // Ping-Pong
            if (f.distance > total) f.distance = total, f.direction = -1.0f;
            if (f.distance < 0.0f) f.distance = 0.0f, f.direction = 1.0f;
        } else {
            f.distance = f.distance < 0.0f ? 0.0f : f.distance > total ? total : f.distance;
        }
        Vec3 tangent;
        const Vec3 at = world::spline_at_distance(points, spline->points.count, spline->closed, f.distance, &tangent, tangents);
        Vec3 t, rotation, scale;
        decompose_trs(world.world_matrix(f.entity), t, rotation, scale);
        if (follower->face_forward) {
            const Vec3 d = tangent * f.direction;
            rotation = {0.0f, -degrees(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y))), degrees(std::atan2(d.y, d.x))};
        }
        world.set_world_matrix(f.entity, trs(at, rotation, scale));
        if (f.body != physics::kWorldBody) physics_->move_kinematic(f.body, translation(at) * rotation_euler(rotation) * translation(mul(f.centre, f.scale)), dt);
    }
}

// Audio Volumes: the one the ears are in sets the reverb (faded); voices on the other side of a
// volume's walls are muffled. Voices whose attenuation has Occlusion are muffled while a physics
// ray from the ears to them is blocked.
void PlaySession::update_audio_volumes(const world::World& world, f32 dt) {
    if (!sounds_) return;
    audio::AudioSystem& a = *sounds_->audio();
    const reflect::TypeInfo& type = reflect::type_of<world::AudioVolume>();
    auto volume_at = [&](Vec3 p) {
        const world::AudioVolume* best = nullptr;
        for (u32 i = 0; i < world.count(type); ++i) {
            const auto* v = static_cast<const world::AudioVolume*>(world.at(type, i));
            if (v->enabled && (!best || v->priority > best->priority) && inside_volume(world, world.owner(type, i), p)) best = v;
        }
        return best;
    };
    const world::AudioVolume* here = volume_at(eye_);
    const f32 target_reverb = here ? here->reverb : 0.0f, target_room = here ? here->room_size : 0.5f;
    const f32 fade = here ? here->fade : 0.5f;
    const f32 step = fade > 0.0f ? dt / fade : 1.0f;
    reverb_now_ += (target_reverb - reverb_now_) * (step > 1.0f ? 1.0f : step);
    room_now_ = target_room;
    a.set_environment(reverb_now_, room_now_);
    for (u32 i = 0; i < a.voice_count(); ++i) {
        const audio::VoiceInfo v = a.voice(i);
        if (!v.spatial) continue;
        const world::AudioVolume* there = volume_at(v.position);
        const world::AudioVolume* wall = here != there ? (here ? here : there) : nullptr;  // between the ears and the sound
        f32 cutoff = wall ? wall->muffle : 20000.0f;
        if (v.occlusion && physics_) {
            const Vec3 to = v.position - eye_;
            const f32 distance = length(to);
            constexpr f32 kSkin = 0.3f;  // stop short: the emitter's own collision does not hide it
            physics::RayHit hit;
            if (distance > kSkin * 2.0f && physics_->raycast(eye_, to * (1.0f / distance), distance - kSkin, hit)) cutoff = std::fmin(cutoff, v.occlusion_muffle);
        }
        // shortcut: the cutoff jumps when the ray is blocked; add a short glide if it clicks on real content
        a.set_voice_lowpass(v.id, cutoff);
    }
}

void PlaySession::send_hits() {
    physics::Contact contacts[256];
    const u32 n = physics_->take_contacts(contacts, 256);
    for (u32 i = 0; i < n; ++i) {
        const world::Entity a = body_entity(contacts[i].a), b = body_entity(contacts[i].b);
        if (!a.valid() || !b.valid()) continue;
        scripts_.send(a, "on_hit", b);
        scripts_.send(b, "on_hit", a);
    }
}

void PlaySession::apply_live_edits(world::World& world) {
    if (world.settings().gravity != live_gravity_) {
        live_gravity_ = world.settings().gravity;
        physics_->set_gravity(live_gravity_);
    }
    // The player's Character: tuning, camera, input, capsule.
    if (const auto* c = world.alive(character_) ? world.get<world::Character>(character_) : nullptr; c && std::memcmp(c, &live_character_, sizeof *c) != 0) {
        const world::Character before = live_character_;
        live_character_ = *c;
        const world::Character resolved = settings_;  // keeps the built-in actions a Character without input assets got
        settings_ = *c;
        if (!wired_) {
            std::memcpy(settings_.move_action, resolved.move_action, sizeof settings_.move_action);
            std::memcpy(settings_.look_action, resolved.look_action, sizeof settings_.look_action);
            std::memcpy(settings_.jump_action, resolved.jump_action, sizeof settings_.jump_action);
        }
        if (std::strcmp(before.mapping_context, c->mapping_context) != 0 && registry_ && load_asset(*registry_, c->mapping_context, context_)) {
            wired_ = true;
            settings_ = *c;
        }
        if (before.height != c->height && !crouched_) physics_->set_character_height(c->height);
        if (before.radius != c->radius || before.max_slope != c->max_slope || before.step_height != c->step_height) {
            physics_->set_character_limits(c->radius, c->max_slope, c->step_height);
        }
    }
    // Input: the Mapping Context or an Input Action edited (keys, triggers): reloaded, rebinds kept.
    // Four checks a second: a few file times, nothing per frame.
    if (wired_ && SDL_GetTicks() - input_checked_ >= 250) {
        input_checked_ = SDL_GetTicks();
        const i64 now = input_files_stamp();
        if (now != input_stamp_ && registry_ && load_asset(*registry_, settings_.mapping_context, context_)) {
            input_stamp_ = input_files_stamp();
            apply_rebinds();
            apply_input();
            LSD_LOG_INFO("play", "Input reloaded (Mapping Context / Input Actions changed)");
        }
    }
    // Rigid bodies: mass, friction, bounciness.
    for (u32 i = 0; i < simulated_count_; ++i) {
        Simulated& s = simulated_[i];
        const auto* rigid = world.alive(s.entity) ? world.get<world::RigidBody>(s.entity) : nullptr;
        if (!rigid) continue;
        if (rigid->friction != s.friction || rigid->bounciness != s.bounciness) {
            s.friction = rigid->friction;
            s.bounciness = rigid->bounciness;
            if (!rigid->physical_material[0]) physics_->set_body_material(s.body, s.friction, s.bounciness);
        }
        if (rigid->mass != s.mass) {
            s.mass = rigid->mass;
            physics_->set_body_mass(s.body, s.mass);
        }
    }
}

void PlaySession::update(world::World& world, const input::InputFrame& frame, f32 dt) {
    net_receive(world, dt);  // other machines' moves and events first
    talk(frame);
    LSD_PROFILE_SCOPE("play");
    if (!physics_) return;
    apply_live_edits(world);  // Details edits made while playing apply now
    dt = dt > 0.05f ? 0.05f : dt;  // a long frame (breakpoint, window drag) must not tunnel through floors
    input_.update(frame, dt);
    char held[32] = "";
    input::held_key(frame, held, sizeof held);
    std::snprintf(key_pressed_, sizeof key_pressed_, "%s", held[0] && std::strcmp(held, key_held_) != 0 ? held : "");
    std::snprintf(key_held_, sizeof key_held_, "%s", held);
    const input::Value move = input_.value(settings_.move_action);
    const input::Value look = input_.value(settings_.look_action);
    const bool aboard = world.alive(driving_) || world.alive(riding_);  // in a vehicle or on a ride: no walking
    const bool jump = !aboard && input_.fired(settings_.jump_action);  // its trigger: Pressed by default, or Hold, Tap...

    const f32 sensitivity = settings_.look_sensitivity * (user_settings_ ? user_settings_->mouse_sensitivity : 1.0f);
    const bool invert = settings_.invert_look_y != (user_settings_ && user_settings_->invert_look);
    yaw_ -= radians(look.x * sensitivity);
    pitch_ += radians(look.y * sensitivity) * (invert ? -1.0f : 1.0f);
    pitch_ = pitch_ < -1.5f ? -1.5f : pitch_ > 1.5f ? 1.5f : pitch_;
    const Vec3 ahead{std::cos(yaw_), std::sin(yaw_), 0};
    const Vec3 right{std::sin(yaw_), -std::cos(yaw_), 0};
    Vec3 velocity = settings_.side_scroller ? Vec3{move.x, 0, 0} : ahead * move.y + right * move.x;  // 2D: along X only
    if (settings_.side_scroller) {  // the player's sprite faces the way it walks and plays idle / walk / jump
        if (auto* sprite = world.get<world::Sprite>(character_)) {
            if (move.x != 0.0f) sprite->flip_x = move.x < 0.0f;
            const bool air = !physics_->character_on_ground() && !swimming_;
            u32 first = idle_frame_, cells = idle_count_;
            if (air && sprite->jump_count) {
                first = sprite->jump_frame;
                cells = sprite->jump_count;
            } else if (move.x != 0.0f && sprite->walk_count) {
                first = sprite->walk_frame;
                cells = sprite->walk_count;
            }
            sprite->frame = first;
            sprite->frame_count = cells;
        }
    }
    const f32 amount = length(velocity);
    if (amount > 1.0f) velocity = velocity * (1.0f / amount);
    if (aboard) velocity = {};
    update_followers(world, dt);  // before the step: moving platforms carry the player and objects
    update_rides(world, frame, move, dt);  // vehicles, transports, movers
    update_sublevels(world);               // level streaming by distance
    for (u32 i = 0; i < simulated_count_; ++i) {  // kinematic rigid bodies go where scripts / timelines put their Transform
        const Simulated& s = simulated_[i];
        if (!s.kinematic || !world.alive(s.entity)) continue;
        Vec3 t, r, scale;
        decompose_trs(world.world_matrix(s.entity), t, r, scale);
        physics_->move_kinematic(s.body, translation(t) * rotation_euler(r) * translation(mul(s.centre, s.scale)), dt);
    }

    // Swimming: the chest inside a Water Volume. Move where you look; Jump (held) rises.
    const Vec3 chest = feet_ + Vec3{0, 0, settings_.height * 0.6f};
    const reflect::TypeInfo& water_type = reflect::type_of<world::WaterVolume>();
    swimming_ = false;
    for (u32 i = 0; i < world.count(water_type) && !swimming_; ++i) {
        swimming_ = static_cast<const world::WaterVolume*>(world.at(water_type, i))->enabled && inside_volume(world, world.owner(water_type, i), chest);
    }
    // Crouching while the crouch action is held; standing up waits until there is room overhead.
    const bool want_crouch = settings_.crouch_action[0] && input_.down(settings_.crouch_action) && !swimming_;
    if (want_crouch != crouched_ && physics_->set_character_height(want_crouch ? settings_.crouch_height : settings_.height)) crouched_ = want_crouch;
    if (swimming_) {
        Vec3 swim = forward() * move.y + right * move.x;
        if (input_.down(settings_.jump_action)) swim.z += 1.0f;
        if (settings_.crouch_action[0] && input_.down(settings_.crouch_action)) swim.z -= 1.0f;  // Crouch held dives
        const f32 n = length(swim);
        if (n > 1.0f) swim = swim * (1.0f / n);
        physics_->move_character(swim * settings_.swim_speed, false, 0.0f, dt, true);
    } else if (Vec3 rooted; animations_ && dt > 0.0f && animations_->take_root_motion(character_, rooted)) {  // a root-motion montage moves it
        physics_->move_character(Vec3{rooted.x / dt, rooted.y / dt, 0.0f}, false, 0.0f, dt);
    } else if (const Moving* car = world.alive(driving_) ? moving_of(driving_) : nullptr; !car || car->vehicle == ~0u) {
        physics_->move_character(velocity * (crouched_ ? settings_.crouch_speed : settings_.walk_speed), jump && !crouched_, settings_.jump_speed, dt);
    }  // in a Physics vehicle the driver sits inside its body: not moved, so it cannot push the car
    physics_->step(dt);
    u32 broken[16];  // breakable joints that snapped: tell the object that owned the joint
    for (u32 n = physics_->take_broken_joints(broken, 16), k = 0; k < n; ++k) {
        if (broken[k] < kMaxJoints && world.alive(joint_owners_[broken[k]])) scripts_.send(joint_owners_[broken[k]], "on_joint_break", {});
    }
    const Vec3 moved = physics_->character_feet() - feet_;
    feet_ = physics_->character_feet();
    place_rider(world);  // a driver or rider stays on its seat
    if (animations_ && character_.valid() && dt > 0.0f) {
        animations_->set_param(world, character_, "Speed", std::sqrt(moved.x * moved.x + moved.y * moved.y) / dt);
        anim::MotionInput motion;  // Motion Matching: where it goes now and where the input takes it
        motion.velocity = {moved.x / dt, moved.y / dt, 0.0f};
        const f32 speed = crouched_ ? settings_.crouch_speed : settings_.walk_speed;
        motion.desired_velocity = aboard ? Vec3{} : Vec3{velocity.x * speed, velocity.y * speed, 0.0f};
        motion.facing = motion.desired_facing = yaw_;
        animations_->set_motion_input(character_, motion);
        animations_->set_param(world, character_, "VerticalSpeed", moved.z / dt);
        animations_->set_param(world, character_, "InAir", physics_->character_on_ground() || swimming_ ? 0.0f : 1.0f);
        animations_->set_param(world, character_, "Crouching", crouched_ ? 1.0f : 0.0f);
        animations_->set_param(world, character_, "Swimming", swimming_ ? 1.0f : 0.0f);
        // Unreal's Calculate Direction: degrees from facing (+X) to the movement, right positive, -180..180.
        const Mat4 facing = world.world_matrix(character_);
        const f32 fx = facing.at(0, 0), fy = facing.at(1, 0);
        const bool moving = moved.x * moved.x + moved.y * moved.y > 1e-8f;
        const f32 direction = moving ? -std::atan2(fx * moved.y - fy * moved.x, fx * moved.x + fy * moved.y) * 57.29578f : 0.0f;
        animations_->set_param(world, character_, "Direction", direction);
    }
    // Objects scripts destroyed leave the simulation.
    for (u32 b = 0; b < physics_->body_count() && b < kMaxBodies; ++b) {
        if (body_entities_[b].valid() && !world.alive(body_entities_[b])) {
            physics_->remove_body(b);
            body_entities_[b] = {};
        }
    }

    // Kill Z and Kill Volumes.
    bool killed = feet_.z < world.settings().kill_z;
    const reflect::TypeInfo& kill_type = reflect::type_of<world::KillVolume>();
    for (u32 i = 0; i < world.count(kill_type) && !killed; ++i) {
        if (!static_cast<const world::KillVolume*>(world.at(kill_type, i))->enabled) continue;
        killed = inside_volume(world, world.owner(kill_type, i), feet_ + Vec3{0, 0, 0.1f});
    }
    if (killed) {
        physics_->teleport_character(spawn_);
        feet_ = spawn_;
        yaw_ = spawn_yaw_;
    }

    // Write the simulation back so the viewport (and anything attached) shows it.
    for (u32 i = 0; i < simulated_count_; ++i) {
        const Simulated& s = simulated_[i];
        if (!world.alive(s.entity) || s.kinematic) continue;  // a script destroyed it; kinematic ones lead, not follow
        world.set_world_matrix(s.entity, physics_->body_transform(s.body) * translation(-mul(s.centre, s.scale)) * scaling(s.scale));
    }
    update_vehicle_wheels(world);
    if (const Moving* car = world.alive(driving_) ? moving_of(driving_) : nullptr; car && car->vehicle != ~0u) place_rider(world);  // on the car where it is now
    if (world.alive(character_)) {
        Vec3 t, r, scale;
        decompose_trs(world.world_matrix(character_), t, r, scale);
        world.set_world_matrix(character_, trs(feet_, {0, 0, degrees(yaw_)}, scale));
    }
    // Camera: the head (or a Camera placed on the Character), moved by Camera Offset, then behind it
    // on the spring arm, pulled in when a wall is in the way. Mouse Look: the mouse turns it (Unreal's
    // Use Pawn Control Rotation) and the Camera's own rotation tilts it; Orbit Bone: it circles a bone;
    // Ride Bone: it moves only with a bone.
    const f32 crouch = crouched_ ? settings_.crouch_height / settings_.height : 1.0f;
    Vec3 head = feet_ + Vec3{0, 0, settings_.eye_height * crouch};
    f32 arm_length = settings_.camera_distance;
    eye_forward_ = forward();
    const world::Entity cam = player_camera(world);
    if (const auto* pc = cam.valid() ? world.get<world::Camera>(cam) : nullptr) {
        const bool child = cam != character_;  // placed in the viewport (on the Character itself: only its settings)
        const Mat4 cam_world = world.world_matrix(cam);
        if (child) head = Vec3{cam_world.at(0, 3), cam_world.at(1, 3), cam_world.at(2, 3)} - Vec3{0, 0, (1.0f - crouch) * (cam_world.at(2, 3) - feet_.z)};
        const u32 view = camera_view_override_ < 2 ? camera_view_override_ : pc->view;
        arm_length = view == 1 ? pc->arm_length : 0.0f;
        Mat4 bone;
        bool found = false;
        if (pc->follow_bone[0] && animations_ && skinned_) {  // a bone of the Character's skeleton (or a child's)
            found = animations_->socket(world, *skinned_, character_, pc->follow_bone, bone);
            for (u32 slot = 0; slot < world.slot_count() && !found; ++slot) {
                const world::Entity e = world.entity_at_slot(slot);
                if (e.valid() && world.parent(e) == character_ && world.get<world::SkeletalMesh>(e)) found = animations_->socket(world, *skinned_, e, pc->follow_bone, bone);
            }
        }
        const Vec3 bone_at = found ? Vec3{bone.at(0, 3), bone.at(1, 3), bone.at(2, 3)} : head;
        if (pc->motion == 2 && found && child) {  // Ride Bone: where it was put relative to the bone, turned with it
            if (!bone_ride_valid_) {
                bone_ride_ = inverse(bone) * cam_world;  // the first frame: how it sits on the bone
                bone_ride_valid_ = true;
            }
            const Mat4 ride = bone * bone_ride_;
            head = {ride.at(0, 3), ride.at(1, 3), ride.at(2, 3)};
            eye_forward_ = normalize(Vec3{ride.at(0, 0), ride.at(1, 0), ride.at(2, 0)});
        } else if (pc->motion == 1) {  // Orbit Bone: the mouse circles the bone
            head = bone_at;
            arm_length = pc->arm_length;
        } else {  // Mouse Look: the Camera's rotation relative to the Character tilts the look
            if (view == 0 && found) head = bone_at;
            if (child && world.alive(character_)) {
                const Mat4 local = inverse(world.world_matrix(character_)) * cam_world;
                const Vec3 x = normalize(Vec3{local.at(0, 0), local.at(1, 0), local.at(2, 0)});
                const Vec3 f = forward(), side = normalize(cross(Vec3{0, 0, 1}, f)), up = cross(f, side);
                const Vec3 turned = f * x.x + side * x.y + up * x.z;  // x is the Camera's forward in the Character's frame
                if (dot(turned, turned) > 1e-6f) eye_forward_ = normalize(turned);
            }
        }
    } else if (camera_view_override_ < 2) {  // no Camera on the Character: a script still switches views
        arm_length = camera_view_override_ == 1 ? (settings_.camera_distance > 0.0f ? settings_.camera_distance : 3.0f) : 0.0f;
    }
    const Vec3 offset = settings_.camera_offset;
    const f32 cy = std::cos(yaw_), sy = std::sin(yaw_);
    head = head + Vec3{cy * offset.x - sy * offset.y, sy * offset.x + cy * offset.y, offset.z};
    eye_ = head;
    if (arm_length > 0.0f) {
        const Vec3 back = eye_forward_ * -1.0f;
        f32 arm = arm_length;
        physics::RayHit hit;
        if (physics_->raycast(head, back, arm, hit)) arm = hit.distance > 0.2f ? hit.distance - 0.2f : 0.0f;
        eye_ = head + back * arm;
    }
    update_timelines(world, dt);
    update_movement(world, dt);
    update_view(world, dt);
    for (u32 i = 0; i < emitter_count_ && sounds_; ++i) {  // emitters follow their objects
        if (!world.alive(emitters_[i].entity)) {
            sounds_->audio()->stop(emitters_[i].voice);
            continue;
        }
        const Mat4 m = world.world_matrix(emitters_[i].entity);
        sounds_->audio()->set_position(emitters_[i].voice, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
    }
    update_audio_volumes(world, dt);
    send_hits();
    update_triggers(world);
    {
        LSD_PROFILE_SCOPE("scripts");
        if (animations_) {  // montages that ended last frame: function on_montage_ended(self)
            static std::vector<world::Entity> ended;
            ended.clear();
            animations_->take_finished_montages(ended);
            for (const world::Entity e : ended) scripts_.send(e, "on_montage_ended", {});
        }
        scripts_.update(dt);
    }
    update_brains(world, dt);
    update_agents(world, dt);
    net_send(world, dt);
}

// ---- Multiplayer ----

namespace {
std::string json_string(const char* text) {  // a JSON string literal
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        const unsigned char ch = static_cast<unsigned char>(*c);
        if (ch == '"' || ch == '\\') out += '\\', out += *c;
        else if (ch < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", ch);
            out += escaped;
        } else out += *c;
    }
    return out + "\"";
}
} // namespace

world::Entity PlaySession::make_avatar(world::World& world, const char* name, Vec3 at, bool mine) {
    // How a player looks to the others: the project's avatar mesh, no collision (players do not
    // block each other: each moves on its own machine). Your own is not drawn for you.
    const world::Entity e = world.create(name);
    world.get<world::Transform>(e)->position = at;
    if (!mine) {
        auto* mesh = world.add<world::StaticMesh>(e);
        mesh->collision = false;
        if (const asset::AssetRecord* r = find_asset(asset::AssetType::Mesh, net_play_.avatar)) asset::to_string(r->guid, mesh->mesh);
        else std::snprintf(mesh->mesh, sizeof mesh->mesh, "%s", net_play_.avatar);
    }
    auto* rep = world.add<world::NetReplicated>(e);
    std::snprintf(rep->spawned_from, sizeof rep->spawned_from, "avatar:%s", net_play_.avatar);
    return e;
}

world::Entity PlaySession::net_spawn(void* user, const char* asset, const char* name, Vec3 position, Vec3 rotation) {
    auto& self = *static_cast<PlaySession*>(user);
    world::World& world = *self.world_;
    world::Entity e;
    if (std::strncmp(asset, "avatar:", 7) == 0) e = self.make_avatar(world, name, position, false);
    else e = self.create_spawn(world, asset, position);
    if (e.valid()) world.get<world::Transform>(e)->rotation = rotation;
    return e;
}

void PlaySession::hear(u32 speaker, const char* channel, const u8* packet, u32 bytes) {
    std::unique_ptr<net::VoiceDecoder>& decoder = decoders_[speaker];
    if (!decoder) decoder = std::make_unique<net::VoiceDecoder>();
    f32 frame[net::kVoiceFrame];
    if (!decoder->decode(packet, bytes, frame)) return;
    f32 gain = 1.0f;
    if (std::strcmp(channel, "proximity") == 0) {  // quieter with distance, silent at the range
        Vec3 there;
        const f32 range = net_play_.settings.proximity_range;
        gain = net_.player_position(speaker, there) && range > 0.0f ? std::clamp(1.0f - length(there - feet_) / range, 0.0f, 1.0f) : 0.0f;
    }
    voice_.play(speaker, frame, gain);
}

// Push-to-talk (or an open mic): the microphone's 20 ms frames, encoded and sent while talking;
// otherwise read and dropped, so nothing old is sent when the key goes down.
void PlaySession::talk(const input::InputFrame& frame) {
    talking_ = false;
    if (!encoder_ || !voice_.capturing()) return;
    auto held = [&](const char* name) {
        const SDL_Scancode k = name[0] ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;
        return k != SDL_SCANCODE_UNKNOWN && frame.keys && static_cast<i32>(k) < frame.key_count && frame.keys[k];
    };
    net::ChatChannel channel = net::ChatChannel::Team;
    bool any = false;
    for (net::ChatChannel c : {net::ChatChannel::Team, net::ChatChannel::Proximity, net::ChatChannel::Squad, net::ChatChannel::All}) {
        if (!any && net_.voice_channel_on(c)) channel = c, any = true;
    }
    if (held(net_play_.talk_to_all_key) && net_.voice_channel_on(net::ChatChannel::All)) channel = net::ChatChannel::All, talking_ = true;
    else talking_ = any && (net_play_.open_mic || held(net_play_.push_to_talk_key));
    f32 pcm[net::kVoiceFrame];
    u8 packet[net::kMaxVoicePacket];
    while (voice_.read_frame(pcm)) {
        if (!talking_) continue;
        if (const u32 bytes = encoder_->encode(pcm, packet, sizeof packet)) net_.send_voice(channel, packet, bytes);
    }
}

bool PlaySession::segment_hit(void* user, Vec3 from, Vec3 to, Vec3& point, Vec3& normal) {
    const auto& self = *static_cast<const PlaySession*>(user);
    const Vec3 d = to - from;
    const f32 len = length(d);
    physics::RayHit hit;
    if (!self.physics_ || len < 1e-6f || !self.physics_->raycast(from, d, len, hit)) return false;
    point = hit.point;
    normal = hit.normal;
    return true;
}

bool PlaySession::send_chat(const char* channel, const char* text) {
    net::ChatChannel c;
    return net_.active() && net::parse_chat_channel(channel, c) && net_.send_chat(c, text);
}

void PlaySession::net_receive(world::World& world, f32 dt) {
    if (!net_.active()) return;
    net_.receive(world, dt);
    net::NetEvent ev;
    while (net_.take_event(ev)) {
        switch (ev.kind) {
        case net::NetEvent::Kind::Call: {
            // Vehicle seats (the engine's own calls): ask, answer, leave.
            u32 id = 0;
            i32 seat = -1;
            auto vehicle_of = [&](u32 net_id) {
                const reflect::TypeInfo& type = reflect::type_of<world::NetReplicated>();
                for (u32 i = 0; i < world.count(type); ++i) {
                    if (static_cast<const world::NetReplicated*>(world.at(type, i))->net_id == net_id) return world.owner(type, i);
                }
                return world::Entity{};
            };
            if (std::strcmp(ev.name, "__seat") == 0 && net_.mode() == net::Mode::Server && std::sscanf(ev.json, "[%u]", &id) == 1) {
                const i32 given = server_take_seat(world, vehicle_of(id), ev.from);
                char args[48];
                std::snprintf(args, sizeof args, "[%u,%d]", id, given);
                net_.send_event(static_cast<i32>(ev.from), "__seated", args);
            } else if (std::strcmp(ev.name, "__unseat") == 0 && net_.mode() == net::Mode::Server) {
                server_free_seats(world, ev.from);
            } else if (std::strcmp(ev.name, "__level") == 0 && net_.mode() == net::Mode::Client) {  // joined: the server's level, if it is another
                const char* open = std::strchr(ev.json, '"');
                const char* close = open ? std::strrchr(ev.json, '"') : nullptr;
                const std::string level = open && close > open ? std::string(open + 1, static_cast<usize>(close - open - 1)) : std::string();
                if (!level.empty() && level != level_name_) {
                    if (const asset::AssetRecord* r = find_asset(asset::AssetType::Level, level.c_str())) {
                        LSD_LOG_INFO("net", "The server is on %s: going there", level.c_str());
                        registry_->source_path(*r, level_request_, sizeof level_request_);
                    } else {
                        LSD_LOG_ERROR("play", "The server is on level '%s', which this machine does not have", level.c_str());
                    }
                }
            } else if (std::strcmp(ev.name, "__travel") == 0 && net_.mode() == net::Mode::Client) {  // the server opened a level: follow
                const char* open = std::strchr(ev.json, '"');
                const char* close = open ? std::strrchr(ev.json, '"') : nullptr;
                const std::string level = open && close > open ? std::string(open + 1, static_cast<usize>(close - open - 1)) : std::string();
                if (const asset::AssetRecord* r = level.empty() ? nullptr : find_asset(asset::AssetType::Level, level.c_str())) {
                    registry_->source_path(*r, level_request_, sizeof level_request_);  // Play restarts there and joins the server again
                } else {
                    LSD_LOG_ERROR("play", "The server opened level '%s', which this machine does not have", level.c_str());
                }
            } else if (std::strcmp(ev.name, "__corrected") == 0) {  // the server put our avatar back (a move it refused)
                f32 x = 0, y = 0, z = 0;
                const auto* rep = world.alive(my_avatar_) ? world.get<world::NetReplicated>(my_avatar_) : nullptr;
                if (rep && std::sscanf(ev.json, "[%u,%f,%f,%f]", &id, &x, &y, &z) == 4 && id == rep->net_id && physics_) {
                    physics_->teleport_character({x, y, z});
                    feet_ = {x, y, z};
                }
                // shortcut: a corrected car (its driver's own) is not put back here; its physics carries on from where the driver had it
            } else if (std::strcmp(ev.name, "__seated") == 0 && std::sscanf(ev.json, "[%u,%d]", &id, &seat) == 2) {
                if (seat >= 0) take_seat(world, vehicle_of(id), seat);
                else LSD_LOG_INFO("play", "That vehicle is full");
            } else {
                scripts_.net_call(ev.name, ev.from, ev.json);
            }
            break;
        }
        case net::NetEvent::Kind::PlayerJoined:
            if (net_.mode() == net::Mode::Server) {  // their avatar, which their machine moves
                char name[32];
                std::snprintf(name, sizeof name, "Player %u", ev.from);
                net_.set_owner(world, make_avatar(world, name, feet_, false), ev.from);
                if (level_name_[0]) net_.send_event(static_cast<i32>(ev.from), "__level", ("[" + json_string(level_name_) + "]").c_str());
            }
            scripts_.net_call("on_player_joined", ev.from, "[]");
            break;
        case net::NetEvent::Kind::PlayerLeft:
            if (net_.mode() == net::Mode::Server) server_free_seats(world, ev.from);  // their car is the server's again
            scripts_.net_call("on_player_left", ev.from, "[]");
            break;
        case net::NetEvent::Kind::Connected: scripts_.net_call("on_connected", ev.from, "[]"); break;
        case net::NetEvent::Kind::Disconnected: scripts_.net_call("on_disconnected", 0, "[]"); break;
        case net::NetEvent::Kind::Voice: hear(ev.from, ev.name, reinterpret_cast<const u8*>(ev.json), ev.size); break;
        case net::NetEvent::Kind::Chat: {
            ChatLine line{};
            line.from = ev.from;
            std::snprintf(line.channel, sizeof line.channel, "%s", ev.name);
            std::snprintf(line.text, sizeof line.text, "%s", ev.json);
            chat_.push_back(line);
            LSD_LOG_INFO("chat", "[%s] Player %u: %s", line.channel, line.from, line.text);
            if (chat_.size() > 100) chat_.erase(chat_.begin());
            const std::string args = "[" + json_string(ev.name) + "," + json_string(ev.json) + "]";
            scripts_.net_call("on_chat", ev.from, args.c_str());
            break;
        }
        }
    }
    // A client's own avatar arrives from the server: found once, hidden here, moved by this machine.
    if (!world.alive(my_avatar_) && net_.mode() == net::Mode::Client && net_.connected()) {
        const reflect::TypeInfo& type = reflect::type_of<world::NetReplicated>();
        for (u32 i = 0; i < world.count(type); ++i) {
            const auto* rep = static_cast<const world::NetReplicated*>(world.at(type, i));
            if (rep->owner != net_.player_id() || std::strncmp(rep->spawned_from, "avatar:", 7) != 0) continue;
            my_avatar_ = world.owner(type, i);
            if (world.get<world::StaticMesh>(my_avatar_)) world.remove(my_avatar_, reflect::type_of<world::StaticMesh>());
            break;
        }
    }
}

void PlaySession::net_send(world::World& world, f32 dt) {
    if (!net_.active()) return;
    if (auto* t = world.alive(my_avatar_) ? world.get<world::Transform>(my_avatar_) : nullptr) {  // where this player is
        t->position = feet_;
        t->rotation = {0.0f, 0.0f, yaw_ * 57.2957795f};
    }
    net_.send(world, dt);
}

struct PlaySession::PlayingTimeline {
    char name[96];
    world::Timeline data;
    f32 time = 0.0f;
    bool loop = false;
    bool cuts = false;  // has camera cuts: the view goes back to the player at the end
};

bool PlaySession::play_timeline(const char* name, bool loop) {
    const asset::AssetRecord* r = find_asset(asset::AssetType::Data, name);
    if (!r || std::strcmp(r->data_type, "Timeline") != 0) {
        LSD_LOG_WARN("timeline", "No Timeline named '%s'", name);
        return false;
    }
    stop_timeline(name);  // from the start again
    PlayingTimeline** slot = nullptr;
    for (PlayingTimeline*& t : timelines_) {
        if (!t && !slot) slot = &t;
    }
    if (!slot) return false;
    auto* t = new PlayingTimeline();
    char path[300];
    registry_->source_path(*r, path, sizeof path);
    if (!asset::load_data_asset(path, t->data)) {
        delete t;
        return false;
    }
    std::snprintf(t->name, sizeof t->name, "%s", name);
    t->loop = loop;
    t->time = -1e-4f;  // keys at 0 fire on the first update
    for (const world::TimelineTrack& track : t->data.tracks) t->cuts = t->cuts || track.type == static_cast<u32>(world::TrackType::CameraCut);
    *slot = t;
    return true;
}

void PlaySession::stop_timeline(const char* name) {
    for (PlayingTimeline*& t : timelines_) {
        if (t && (!name || std::strcmp(t->name, name) == 0)) {
            if (t->cuts) view_camera_ = {};
            delete t;
            t = nullptr;
        }
    }
}

// Timelines: poses, then the keys this frame crossed (events, sounds, camera cuts).
void PlaySession::update_timelines(world::World& world, f32 dt) {
    for (PlayingTimeline*& t : timelines_) {
        if (!t) continue;
        const f32 from = t->time;
        t->time += dt;
        const f32 length = t->data.length > 0.01f ? t->data.length : 0.01f;
        const bool ended = t->time >= length;
        const f32 to = ended ? length : t->time;
        pose_timeline(world, t->data, to);
        for (u32 i = 0; i < t->data.tracks.count; ++i) {
            const auto type = static_cast<world::TrackType>(t->data.tracks[i].type);
            if (type == world::TrackType::Transform || type == world::TrackType::Property) continue;
            const world::TimelineKey* keys[32];
            const u32 n = world::keys_crossed(t->data, i, from, to, keys, 32);
            for (u32 k = 0; k < n; ++k) {
                if (type == world::TrackType::Event) {
                    scripts_.broadcast(keys[k]->text, t->name);
                } else if (type == world::TrackType::Sound && sounds_) {
                    const world::Entity at = find_named(world, t->data.tracks[i].object);
                    const Mat4 m = world.alive(at) ? world.world_matrix(at) : Mat4{};
                    sounds_->play(keys[k]->text, world.alive(at), {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
                } else if (type == world::TrackType::CameraCut) {
                    view_camera_ = find_named(world, keys[k]->text);
                }
            }
        }
        if (!ended) continue;
        if (t->loop) {
            t->time = -1e-4f;
            continue;
        }
        char name[96];
        std::snprintf(name, sizeof name, "%s", t->name);
        if (t->cuts) view_camera_ = {};
        delete t;
        t = nullptr;
        scripts_.broadcast("on_timeline_finished", name);  // function on_timeline_finished(self, name)
    }
}

// Rotating and Projectile Movement components.
void PlaySession::update_movement(world::World& world, f32 dt) {
    const reflect::TypeInfo& rotating = reflect::type_of<world::RotatingMovement>();
    for (u32 i = 0; i < world.count(rotating); ++i) {
        const auto* m = static_cast<const world::RotatingMovement*>(world.at(rotating, i));
        if (auto* t = world.get<world::Transform>(world.owner(rotating, i))) t->rotation = t->rotation + m->rate * dt;
    }
    const reflect::TypeInfo& projectile = reflect::type_of<world::ProjectileMovement>();
    for (u32 i = 0; i < world.count(projectile); ++i) {  // newcomers (placed or spawned) start flying
        const world::Entity e = world.owner(projectile, i);
        bool known = false;
        for (u32 k = 0; k < projectile_count_ && !known; ++k) known = projectiles_[k].entity == e;
        if (known || projectile_count_ == sizeof projectiles_ / sizeof projectiles_[0]) continue;
        const auto* p = static_cast<const world::ProjectileMovement*>(world.at(projectile, i));
        const Mat4 m = world.world_matrix(e);
        const Vec3 axes[3] = {normalize(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)}), normalize(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}),
                              normalize(Vec3{m.at(0, 2), m.at(1, 2), m.at(2, 2)})};
        projectiles_[projectile_count_++] = {e, axes[0] * p->velocity.x + axes[1] * p->velocity.y + axes[2] * p->velocity.z, true};
    }
    const f32 gravity = world.settings().gravity;
    for (u32 k = 0; k < projectile_count_; ++k) {
        Projectile& pr = projectiles_[k];
        const auto* p = world.alive(pr.entity) ? world.get<world::ProjectileMovement>(pr.entity) : nullptr;
        if (!p) {  // gone or no longer a projectile
            projectiles_[k--] = projectiles_[--projectile_count_];
            continue;
        }
        if (!pr.flying) continue;
        pr.velocity.z -= gravity * p->gravity_scale * dt;
        const Mat4 m = world.world_matrix(pr.entity);
        const Vec3 from{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        const f32 speed = length(pr.velocity);
        Vec3 to = from + pr.velocity * dt;
        physics::RayHit hit;
        if (speed > 1e-4f && physics_->raycast(from, pr.velocity * (1.0f / speed), speed * dt, hit)) {
            to = hit.point;
            pr.flying = false;
            const world::Entity other = body_entity(hit.body);
            scripts_.send(pr.entity, "on_hit", other);
            if (world.alive(other)) scripts_.send(other, "on_hit", pr.entity);
        }
        Mat4 moved = m;
        moved.at(0, 3) = to.x;
        moved.at(1, 3) = to.y;
        moved.at(2, 3) = to.z;
        world.set_world_matrix(pr.entity, moved);
        if (p->rotation_follows_velocity && speed > 0.1f) {
            if (auto* t = world.get<world::Transform>(pr.entity)) {
                t->rotation.y = -degrees(std::asin(std::clamp(pr.velocity.z / speed, -1.0f, 1.0f)));
                t->rotation.z = degrees(std::atan2(pr.velocity.y, pr.velocity.x));
            }
        }
        if (!pr.flying && p->destroy_on_hit) world.destroy(pr.entity);
    }
}

void PlaySession::set_view(world::Entity camera, f32 blend_seconds) {
    view_camera_ = camera;
    blend_eye_ = view_eye_;  // from what is on screen now (mid-blend too)
    blend_forward_ = view_forward_;
    blend_fov_ = view_fov_;
    blend_time_ = 0.0f;
    blend_length_ = blend_seconds > 0.0f ? blend_seconds : 0.0f;
}

void PlaySession::camera_shake(f32 strength, f32 seconds, f32 frequency) {
    shake_strength_ = strength;
    shake_time_ = 0.0f;
    shake_length_ = seconds > 0.0f ? seconds : 0.0f;
    shake_frequency_ = frequency > 0.0f ? frequency : 12.0f;
}

// Where the game is seen from: a Camera picked by lsd.set_view or Auto Activate, else the player;
// then the blend from the previous view and any camera shake on top.
void PlaySession::update_view(const world::World& world, f32 dt) {
    find_view(world);
    vehicle_view(world);
    view_clock_ += dt;
    if (blend_time_ < blend_length_) {
        blend_time_ += dt;
        const f32 x = blend_time_ >= blend_length_ ? 1.0f : blend_time_ / blend_length_;
        const f32 t = x * x * (3.0f - 2.0f * x);  // ease in and out
        view_eye_ = blend_eye_ + (view_eye_ - blend_eye_) * t;
        view_forward_ = normalize(blend_forward_ + (view_forward_ - blend_forward_) * t);
        if (blend_fov_ > 0.0f && view_fov_ > 0.0f) view_fov_ = blend_fov_ + (view_fov_ - blend_fov_) * t;  // 0 = the editor's lens: no blend
    }
    if (shake_time_ < shake_length_) {
        shake_time_ += dt;
        const f32 amount = shake_strength_ * (1.0f - shake_time_ / shake_length_);
        const f32 w = 6.2831853f * shake_frequency_ * view_clock_;
        view_eye_ = view_eye_ + Vec3{std::sin(w), std::sin(w * 1.31f + 1.7f), std::sin(w * 0.77f + 3.1f)} * (0.05f * amount);
        const Vec3 side = normalize(cross(view_forward_, Vec3{0, 0, 1}));
        const Vec3 up = cross(side, view_forward_);
        view_forward_ = normalize(view_forward_ + side * (std::sin(w * 1.13f + 0.4f) * 0.0175f * amount) + up * (std::sin(w * 0.91f + 2.2f) * 0.0175f * amount));
    }
}

void PlaySession::find_view(const world::World& world) {
    view_eye_ = eye_;
    view_forward_ = eye_forward_;
    view_fov_ = 0.0f;
    view_ortho_ = 0.0f;
    const reflect::TypeInfo& camera_type = reflect::type_of<world::Camera>();
    world::Entity camera = view_camera_;
    if (!world.alive(camera) || !world.get<world::Camera>(camera)) {
        camera = {};
        for (u32 i = 0; i < world.count(camera_type) && !camera.valid(); ++i) {
            if (static_cast<const world::Camera*>(world.at(camera_type, i))->auto_activate) camera = world.owner(camera_type, i);
        }
    }
    if (!camera.valid()) {
        if (const world::Entity own = player_camera(world); own.valid()) view_fov_ = radians(world.get<world::Camera>(own)->fov);  // the player's Camera: its lens
        return;
    }
    const Mat4 m = world.world_matrix(camera);
    view_eye_ = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    view_forward_ = normalize(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
    view_fov_ = radians(world.get<world::Camera>(camera)->fov);
    if (const auto* cam = world.get<world::Camera>(camera); cam->orthographic) view_ortho_ = cam->ortho_height;
}

// shortcut: point tests (the Character's middle, a body's centre), not shape overlaps; use Jolt sensors for big objects.
void PlaySession::update_triggers(world::World& world) {
    const reflect::TypeInfo& trigger_type = reflect::type_of<world::TriggerVolume>();
    Overlap now[sizeof overlaps_ / sizeof overlaps_[0]];
    u32 now_count = 0;
    auto test = [&](world::Entity other, Vec3 point) {
        for (u32 i = 0; i < world.count(trigger_type) && now_count < sizeof now / sizeof now[0]; ++i) {
            const world::Entity trigger = world.owner(trigger_type, i);
            if (trigger != other && static_cast<const world::TriggerVolume*>(world.at(trigger_type, i))->enabled && inside_volume(world, trigger, point)) {
                now[now_count++] = {trigger, other};
            }
        }
    };
    if (world.alive(character_)) test(character_, feet_ + Vec3{0, 0, settings_.height * 0.5f});
    for (u32 i = 0; i < simulated_count_; ++i) {
        if (!world.alive(simulated_[i].entity)) continue;
        const Mat4 body = physics_->body_transform(simulated_[i].body);
        test(simulated_[i].entity, {body.at(0, 3), body.at(1, 3), body.at(2, 3)});
    }
    auto contains = [](const Overlap* list, u32 count, const Overlap& o) {
        for (u32 i = 0; i < count; ++i) {
            if (list[i].trigger == o.trigger && list[i].other == o.other) return true;
        }
        return false;
    };
    const Overlap* before = overlaps_;
    const u32 before_count = overlap_count_;
    Overlap left[sizeof overlaps_ / sizeof overlaps_[0]];
    u32 left_count = 0;
    for (u32 i = 0; i < before_count; ++i) {
        if (!contains(now, now_count, before[i])) left[left_count++] = before[i];
    }
    Overlap entered[sizeof overlaps_ / sizeof overlaps_[0]];
    u32 entered_count = 0;
    for (u32 i = 0; i < now_count; ++i) {
        if (!contains(before, before_count, now[i])) entered[entered_count++] = now[i];
    }
    // State first: scripts reacting to the events may destroy objects or move them.
    std::memcpy(overlaps_, now, now_count * sizeof now[0]);
    overlap_count_ = now_count;
    for (u32 i = 0; i < left_count; ++i) {
        scripts_.send(left[i].trigger, "on_overlap_end", left[i].other);
        scripts_.send(left[i].other, "on_overlap_end", left[i].trigger);
    }
    for (u32 i = 0; i < entered_count; ++i) {
        const auto* trigger = world.get<world::TriggerVolume>(entered[i].trigger);
        if (trigger && trigger->enter_sound[0] && sounds_) {
            const Mat4 m = world.world_matrix(entered[i].trigger);
            sounds_->play(trigger->enter_sound, true, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        }
        scripts_.send(entered[i].trigger, "on_overlap_begin", entered[i].other);
        scripts_.send(entered[i].other, "on_overlap_begin", entered[i].trigger);
    }
}

// ---- Sub Levels (level streaming) ----

bool PlaySession::stream_sublevel(world::World& world, world::Entity root, bool load) {
    const auto* sub = world.alive(root) ? world.get<world::SubLevel>(root) : nullptr;
    if (!sub) return false;
    const bool loaded = world::sublevel_loaded(world, root);
    if (!load) {
        if (loaded) world::unload_sublevel(world, root);  // their bodies go on the next step (dead objects)
        return true;
    }
    if (loaded) return true;
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(sub->level, guid) ? registry_->find(guid) : nullptr;
    if (!r || r->type != asset::AssetType::Level) return false;
    char path[300];
    registry_->source_path(*r, path, sizeof path);
    if (!world::load_sublevel(world, root, path)) return false;
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {  // what came in: bodies, then scripts
        const world::Entity e = world.entity_at_slot(slot);
        if (!e.valid()) continue;
        bool inside = false;  // under one of the root's (just loaded) transient objects
        for (world::Entity a = e; a.valid() && !inside; a = world.parent(a)) inside = world.parent(a) == root && world.transient(a);
        if (inside) add_entity_bodies(world, e);
    }
    scripts_.attach(root);
    return true;
}

// Streaming by distance: in within Load Distance, out past 1.2 times it (no flicker at the edge).
void PlaySession::update_sublevels(world::World& world) {
    const reflect::TypeInfo& type = reflect::type_of<world::SubLevel>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const world::Entity e = world.owner(type, i);
        const auto* sub = static_cast<const world::SubLevel*>(world.at(type, i));
        if (sub->load_distance <= 0.0f || world.transient(world.parent(e))) continue;
        const Mat4 m = world.world_matrix(e);
        const f32 d = length(Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - feet_);
        const bool loaded = world::sublevel_loaded(world, e);
        if (!loaded && d < sub->load_distance) stream_sublevel(world, e, true);
        else if (loaded && d > sub->load_distance * 1.2f) stream_sublevel(world, e, false);
    }
}

// ---- Vehicles, transports and movers ----

namespace {
// The nearest point of an object's unit box (a scaled cube placeholder or mesh) to `p`, in the world.
Vec3 nearest_on_box(const world::World& world, world::Entity e, Vec3 p) {
    const Mat4 m = world.world_matrix(e);
    Vec3 local = transform_point(affine_inverse(m), p);
    local = {std::clamp(local.x, kUnitLo.x, kUnitHi.x), std::clamp(local.y, kUnitLo.y, kUnitHi.y), std::clamp(local.z, kUnitLo.z, kUnitHi.z)};
    return transform_point(m, local);
}
Vec3 axis_x(const Mat4& m) { return normalize(Vec3{m.at(0, 0), m.at(1, 0), 0.0f}); }
} // namespace

void PlaySession::add_moving(const world::World& world, world::Entity e, const CachedMesh* cached, bool solid) {
    Moving m;
    m.entity = e;
    Vec3 t, r, s;
    decompose_trs(world.world_matrix(e), t, r, s);
    m.start_position = t;
    m.start_rotation = r;
    m.world_scale = s;
    m.yaw = radians(r.z);
    if (solid) m.body = add_body(e, fitted_body(world.world_matrix(e), cached ? cached->bounds_min : kUnitLo, cached ? cached->bounds_max : kUnitHi, 0, physics::Motion::Kinematic, m.scale, m.centre));
    if (const auto* mover = world.get<world::Mover>(e)) {
        m.running = mover->start == 0;  // Begin Play
        if (m.running) m.started_at = 0.0;
        for (u32 slot = 0; slot < world.slot_count() && mover->path == 2; ++slot) {
            const world::Entity candidate = world.entity_at_slot(slot);
            if (candidate.valid() && world.get<world::Spline>(candidate) && std::strcmp(world.name(candidate), mover->spline) == 0) m.spline = candidate;
        }
        if (mover->path == 2 && !m.spline.valid()) LSD_LOG_WARN("play", "%s: Mover: no object named '%s' with a Spline", world.name(e), mover->spline);
    }
    moving_.push_back(m);
}

// A Physics vehicle: a dynamic box fitted to its mesh, its centre of mass low (it should not tip at
// every corner), on its listed wheels or four at the corners. The body writes its Transform back.
void PlaySession::add_physics_vehicle(const world::World& world, world::Entity e, const CachedMesh* cached, const world::Vehicle& v) {
    if (simulated_count_ == sizeof simulated_ / sizeof simulated_[0]) return;
    Vec3 scale, centre;
    physics::BodyDesc d = fitted_body(world.world_matrix(e), cached ? cached->bounds_min : kUnitLo, cached ? cached->bounds_max : kUnitHi, 0,
                                      physics::Motion::Dynamic, scale, centre);
    d.mass = v.mass;
    d.friction = 0.3f;  // the body sliding on walls; the tyres have their own grip
    d.centre_of_mass_offset = {0.0f, 0.0f, -d.half_extents.z * 0.9f};
    const u32 body = add_body(e, d);
    simulated_[simulated_count_++] = {e, body, scale, centre, false, v.mass, d.friction, 0.0f};

    physics::VehicleDesc vd;
    vd.body = body;
    vd.engine_torque = v.engine_torque;
    vd.max_rpm = v.max_rpm;
    vd.brake_torque = v.brake_torque;
    auto wheel = [&](Vec3 at, f32 radius, bool steers, bool driven, bool handbrake) {
        physics::WheelDesc& w = vd.wheels[vd.wheel_count++];
        w.position = at;
        w.radius = radius;
        w.width = v.wheel_width;
        w.suspension_length = v.suspension_length;
        w.suspension_frequency = v.suspension_stiffness;
        w.suspension_damping = v.suspension_damping;
        w.max_steer_degrees = steers ? v.max_steer_angle : 0.0f;
        w.driven = driven;
        w.handbrake = handbrake;
    };
    Moving m;
    m.entity = e;
    m.body = body;
    m.world_scale = scale;
    const Vec3 body_centre = mul(centre, scale);  // the body sits at the box's centre, the wheels are given from the origin
    if (v.wheels.count) {
        for (u32 i = 0; i < v.wheels.count && vd.wheel_count < physics::kMaxWheels; ++i) {
            const world::VehicleWheel& w = v.wheels[i];
            wheel(w.position - body_centre, w.radius > 0.0f ? w.radius : v.wheel_radius, w.steers, w.driven, w.handbrake);
            for (u32 slot = 0; w.object[0] && slot < world.slot_count(); ++slot) {  // its wheel mesh: a child (any depth) of that name
                const world::Entity c = world.entity_at_slot(slot);
                if (!c.valid() || c == e || std::strcmp(world.name(c), w.object) != 0) continue;
                for (world::Entity p = world.parent(c); p.valid(); p = world.parent(p)) {
                    if (p == e) m.wheel_objects[i] = c;
                }
            }
            if (w.object[0] && !m.wheel_objects[i].valid()) LSD_LOG_WARN("play", "%s: wheel %u: no child object named '%s'", world.name(e), i, w.object);
        }
    } else {  // four wheels under the corners of its box, the front ones steering
        const Vec3 h = d.half_extents;
        const f32 x = std::max(h.x - v.wheel_radius * 1.2f, h.x * 0.3f), y = std::max(h.y - v.wheel_width * 0.5f, h.y * 0.3f), z = -h.z * 0.75f;
        for (u32 i = 0; i < 4; ++i) {
            const bool front = i < 2;
            wheel({front ? x : -x, i % 2 == 0 ? y : -y, z}, v.wheel_radius, front, v.drive == 2 || (v.drive == 1) == front, !front);
        }
    }
    m.wheel_count = vd.wheel_count;
    m.vehicle = physics_->add_vehicle(vd);
    physics_->drive_vehicle(m.vehicle, 0.0f, 0.0f, 0.0f, 1.0f);  // parked
    m.parked = true;
    moving_.push_back(m);
}

// ---- Multiplayer seats: the server decides who sits where; the driver's machine owns the car. ----

i32 PlaySession::server_take_seat(world::World& world, world::Entity vehicle, u32 player) {
    const auto* v = world.alive(vehicle) ? world.get<world::Vehicle>(vehicle) : nullptr;
    if (!v) return -1;
    server_free_seats(world, player);  // one seat per player
    Seats* s = nullptr;
    for (Seats& candidate : seats_) s = candidate.vehicle == vehicle ? &candidate : s;
    if (!s) {
        seats_.push_back({vehicle, {}});
        s = &seats_.back();
        for (i64& p : s->player) p = -1;
    }
    const u32 count = 1 + std::min<u32>(v->passenger_seats.count, 8);
    for (u32 seat = 0; seat < count; ++seat) {
        if (s->player[seat] >= 0) continue;
        s->player[seat] = player;
        if (seat == 0) net_.set_owner(world, vehicle, player);  // the driver's machine simulates it
        return static_cast<i32>(seat);
    }
    return -1;
}

void PlaySession::server_free_seats(world::World& world, u32 player) {
    for (Seats& s : seats_) {
        for (u32 seat = 0; seat < 9; ++seat) {
            if (s.player[seat] != static_cast<i64>(player)) continue;
            s.player[seat] = -1;
            if (seat == 0 && world.alive(s.vehicle)) net_.set_owner(world, s.vehicle, net::kServerPlayer);  // parked: the server's again
        }
    }
}

void PlaySession::take_seat(world::World& world, world::Entity vehicle, i32 seat) {
    if (!world.alive(vehicle) || seat < 0) return;
    driving_ = riding_ = {};
    passenger_seat_ = 0;
    if (seat == 0) driving_ = vehicle;
    else {
        riding_ = vehicle;
        passenger_seat_ = static_cast<u32>(seat);
    }
    scripts_.send(vehicle, "on_enter", character_);
}

// Physics cars in multiplayer: dynamic on the machine that owns them (the driver's, or the server's
// when parked), kinematic everywhere else, following the replicated Transform.
void PlaySession::update_vehicle_authority(world::World& world) {
    for (Moving& m : moving_) {
        if (m.vehicle == ~0u || !world.alive(m.entity)) continue;
        const auto* rep = net_.active() ? world.get<world::NetReplicated>(m.entity) : nullptr;
        const bool here = !rep || rep->owner == net_.player_id();
        for (u32 i = 0; i < simulated_count_; ++i) {
            Simulated& s = simulated_[i];
            if (s.entity != m.entity || s.kinematic == !here) continue;
            s.kinematic = !here;
            physics_->set_body_kinematic(m.body, !here);
            if (here) m.parked = false;  // parks again (handbrake) unless someone drives it
        }
    }
}

// Driving a Physics vehicle: W / S throttle and reverse (pressing against the way it rolls brakes
// first), A / D steer, Ctrl brake, Space handbrake, Shift boost (more engine).
void PlaySession::drive_physics(Moving& m, const world::Vehicle& v, const input::InputFrame& frame, input::Value move) {
    auto key = [&](SDL_Scancode k) { return frame.keys && static_cast<i32>(k) < frame.key_count && frame.keys[k]; };
    const physics::VehicleState state = physics_->vehicle_state(m.vehicle);
    f32 forward = move.y, brake = key(SDL_SCANCODE_LCTRL) ? 1.0f : 0.0f;
    if ((forward > 0.01f && state.speed < -0.5f) || (forward < -0.01f && state.speed > 0.5f)) {
        brake = 1.0f;
        forward = 0.0f;
    }
    physics_->drive_vehicle(m.vehicle, forward, move.x, brake, key(SDL_SCANCODE_SPACE) ? 1.0f : 0.0f, key(SDL_SCANCODE_LSHIFT) ? v.boost : 1.0f);
    m.parked = false;
}

void PlaySession::update_vehicle_wheels(world::World& world) {
    for (const Moving& m : moving_) {
        if (m.vehicle == ~0u || !world.alive(m.entity)) continue;
        const auto* v = world.get<world::Vehicle>(m.entity);
        for (u32 i = 0; i < m.wheel_count; ++i) {
            const Mat4 at = physics_->wheel_transform(m.vehicle, i);
            if (world.alive(m.wheel_objects[i])) {
                Vec3 t, r, s;
                decompose_trs(world.world_matrix(m.wheel_objects[i]), t, r, s);
                world.set_world_matrix(m.wheel_objects[i], at * scaling(s));
            } else if (v) {  // no wheel mesh: a ring shows where it is (the placeholder box has no wheels)
                const f32 radius = i < v->wheels.count && v->wheels[i].radius > 0.0f ? v->wheels[i].radius : v->wheel_radius;
                Vec3 last{};
                for (int k = 0; k <= 16; ++k) {
                    const f32 a = static_cast<f32>(k) * 6.2831853f / 16.0f;
                    const Vec3 p = transform_point(at, {std::cos(a) * radius, 0.0f, std::sin(a) * radius});
                    if (k) debug_draw::line(last, p, {0.15f, 0.15f, 0.15f}, 0.0f);
                    last = p;
                }
            }
        }
    }
}

PlaySession::Moving* PlaySession::moving_of(world::Entity e) {
    for (Moving& m : moving_) {
        if (m.entity == e) return &m;
    }
    return nullptr;
}

void PlaySession::restart_mover(world::World& world, Moving& m) {
    m.time = m.distance = m.wait = 0.0f;
    m.direction = 1.0f;
    m.spun = {};
    m.running = true;
    m.started_at = play_clock_;
    world.set_world_matrix(m.entity, trs(m.start_position, m.start_rotation, m.world_scale));
}

// One step of a Mover: where its path puts it now (false: it does not move this frame).
bool PlaySession::step_mover(world::World& world, Moving& m, const world::Mover& mover, f32 dt, Vec3& at, Vec3& rotation) {
    if (m.wait > 0.0f) {  // resting at an end (a station, a lift floor)
        m.wait -= dt;
        return false;
    }
    rotation = m.start_rotation + (m.spun = m.spun + mover.spin * dt);
    auto ends = [&](f32 length) {  // Once / Loop / Ping-Pong at the ends of a route `length` long
        if (m.distance >= length || m.distance <= 0.0f) {
            const bool at_end = m.distance >= length;
            if (mover.repeat == 0) {
                m.distance = std::clamp(m.distance, 0.0f, length);
                if (at_end) m.running = false;
            } else if (mover.repeat == 1) {
                if (at_end) m.distance = length > 0.0f ? std::fmod(m.distance, length) : 0.0f, m.wait = mover.pause;
            } else {
                m.distance = std::clamp(m.distance, 0.0f, length);
                m.direction = at_end ? -1.0f : 1.0f;
                m.wait = mover.pause;
            }
        }
    };
    switch (mover.path) {
    case 0:  // a direction, forever or for Duration seconds
        m.time += dt;
        if (mover.duration > 0.0f && m.time >= mover.duration) {
            m.time = mover.duration;
            m.running = false;
        }
        at = m.start_position + mover.velocity * m.time;
        return true;
    case 1: {  // back and forth to Offset
        const f32 length = std::sqrt(mover.offset.x * mover.offset.x + mover.offset.y * mover.offset.y + mover.offset.z * mover.offset.z);
        if (length < 1e-4f) return false;
        m.distance += mover.speed * m.direction * dt;
        ends(length);
        at = m.start_position + mover.offset * (m.distance / length);
        return true;
    }
    default: {  // along a Spline object's route
        const auto* spline = world.alive(m.spline) ? world.get<world::Spline>(m.spline) : nullptr;
        if (!spline || spline->points.count < 2) return false;
        Vec3 points[32], tangents[32];
        bool closed = false;
        world::spline_world_curve(world, m.spline, points, tangents, closed);
        const f32 length = world::spline_length(points, spline->points.count, spline->closed, tangents);
        m.distance += mover.speed * m.direction * dt;
        ends(length);
        Vec3 tangent;
        at = world::spline_at_distance(points, spline->points.count, spline->closed, m.distance, &tangent, tangents);
        if (mover.face_forward) {
            const Vec3 d = tangent * m.direction;
            rotation = Vec3{0.0f, -degrees(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y))), degrees(std::atan2(d.y, d.x))} + m.spun;
        }
        return true;
    }
    }
}

// A driven vehicle, arcade-style: throttle / reverse / brake / boost, steering that needs speed,
// ground following by ray, walls stop it.
void PlaySession::drive(world::World& world, Moving& m, const world::Vehicle& v, const input::InputFrame& frame, input::Value move, f32 dt, Vec3& at) {
    auto key = [&](SDL_Scancode k) { return frame.keys && static_cast<i32>(k) < frame.key_count && frame.keys[k]; };
    const bool boost = key(SDL_SCANCODE_LSHIFT);
    const bool brake = key(SDL_SCANCODE_LCTRL) || (v.kind == 0 && key(SDL_SCANCODE_SPACE));
    const f32 top = v.max_speed * (boost ? v.boost : 1.0f);
    auto toward = [](f32 value, f32 target, f32 step) { return value < target ? std::min(value + step, target) : std::max(value - step, target); };
    if (move.y > 0.01f) m.speed = m.speed < 0.0f ? toward(m.speed, 0.0f, v.braking * dt) : std::min(m.speed + v.acceleration * move.y * dt, top);
    else if (move.y < -0.01f) m.speed = m.speed > 0.0f ? toward(m.speed, 0.0f, v.braking * dt) : std::max(m.speed + v.acceleration * move.y * dt, -v.reverse_speed);
    else m.speed = toward(m.speed, 0.0f, v.acceleration * 0.5f * dt);  // rolling to a stop
    if (brake) m.speed = toward(m.speed, 0.0f, v.braking * 1.5f * dt);
    if (m.speed > top) m.speed = toward(m.speed, top, v.braking * dt);  // boost released
    m.yaw -= radians(v.turn_rate) * move.x * dt * std::clamp(m.speed / 4.0f, -1.0f, 1.0f);  // turning needs some speed; reversing turns the other way
    const Vec3 forward{std::cos(m.yaw), std::sin(m.yaw), 0.0f};
    const Vec3 from = at;
    Vec3 next = from + forward * (m.speed * dt);
    // Walls: a ray from just outside the nose (or tail) along the travel.
    const f32 half_length = m.world_scale.x * 0.5f;
    if (std::fabs(m.speed) > 0.01f) {
        const f32 sign = m.speed > 0.0f ? 1.0f : -1.0f;
        physics::RayHit hit;
        const Vec3 nose = from + forward * (sign * (half_length + 0.05f)) + Vec3{0, 0, std::max(0.3f, m.world_scale.z * 0.5f)};
        if (physics_->raycast(nose, forward * sign, std::fabs(m.speed * dt) + 0.1f, hit, m.body)) {
            m.speed = 0.0f;
            next = from;
        }
    }
    if (v.kind == 1) {  // flying: Space climbs, Ctrl sinks
        next.z += ((key(SDL_SCANCODE_SPACE) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_LCTRL) ? 1.0f : 0.0f)) * v.climb_speed * dt;
    } else {  // ground: down to what is below (steps up to 0.5 m), or falls
        physics::RayHit hit;  // from half a metre up (steps and ramps), through the vehicle itself
        const bool found = physics_->raycast(next + Vec3{0, 0, 0.5f}, {0, 0, -1}, 50.0f, hit, m.body);
        const f32 ground = found ? hit.point.z : -1e9f;
        if (next.z - ground > 0.05f) {
            m.fall += world.settings().gravity * dt;
            next.z = std::max(ground, next.z - m.fall * dt);
        } else {
            m.fall = 0.0f;
            next.z = ground;
        }
    }
    at = next;
}

void PlaySession::update_rides(world::World& world, const input::InputFrame& frame, input::Value move, f32 dt) {
    play_clock_ += dt;
    // E gets in / out of the nearest vehicle or transport.
    const bool e_down = frame.keys && SDL_SCANCODE_E < frame.key_count && frame.keys[SDL_SCANCODE_E];
    if (e_down && !interact_was_down_) {
        if (world.alive(driving_) || world.alive(riding_)) {
            ride_command(world, world.alive(driving_) ? 1 : 4, {});
        } else {
            world::Entity best;
            f32 best_d = 1e30f;
            for (const Moving& m : moving_) {
                const auto* v = world.get<world::Vehicle>(m.entity);
                const auto* t = world.get<world::Transport>(m.entity);
                if (!v && !t) continue;
                const f32 d = length(nearest_on_box(world, m.entity, feet_ + Vec3{0, 0, 0.9f}) - (feet_ + Vec3{0, 0, 0.9f}));
                if (d <= (v ? v->enter_distance : t->enter_distance) && d < best_d) best = m.entity, best_d = d;
            }
            if (best.valid()) ride_command(world, world.get<world::Vehicle>(best) ? 0 : 3, best);
        }
    }
    interact_was_down_ = e_down;

    update_vehicle_authority(world);
    const reflect::TypeInfo& trigger_type = reflect::type_of<world::TriggerVolume>();
    for (Moving& m : moving_) {
        if (!world.alive(m.entity)) continue;
        const auto* rep = net_.active() ? world.get<world::NetReplicated>(m.entity) : nullptr;
        if (rep && rep->owner != net_.player_id()) {  // another machine moves it: its body follows the replicated Transform
            if (m.body != physics::kWorldBody && m.vehicle == ~0u) {
                Vec3 t, r, s;
                decompose_trs(world.world_matrix(m.entity), t, r, s);
                physics_->move_kinematic(m.body, translation(t) * rotation_euler(r) * translation(mul(m.centre, m.scale)), dt);
            }
            continue;  // (physics cars: the kinematic-body pass moves them)
        }
        Vec3 t, rotation, scale;
        decompose_trs(world.world_matrix(m.entity), t, rotation, scale);
        Vec3 at = t;
        bool moved = false;
        if (const auto* mover = world.get<world::Mover>(m.entity)) {
            // Starting: a Trigger the player enters, or a time of the game; then again every N minutes.
            if (!m.running && m.started_at < 0.0) {
                if (mover->start == 2 && play_clock_ >= mover->start_time) restart_mover(world, m);
                for (u32 i = 0; mover->start == 1 && i < world.count(trigger_type); ++i) {
                    const world::Entity trigger = world.owner(trigger_type, i);
                    if (std::strcmp(world.name(trigger), mover->trigger) == 0 && inside_volume(world, trigger, feet_ + Vec3{0, 0, 0.9f})) restart_mover(world, m);
                }
            }
            if (mover->repeat_minutes > 0.0f && m.started_at >= 0.0 && play_clock_ - m.started_at >= mover->repeat_minutes * 60.0) restart_mover(world, m);
            if (m.running) moved = step_mover(world, m, *mover, dt, at, rotation);
        }
        if (m.vehicle != ~0u) {  // a Physics vehicle: its body moves it
            if (const auto* v = world.get<world::Vehicle>(m.entity); v && m.entity == driving_) drive_physics(m, *v, frame, move);
            else if (!m.parked) {
                physics_->drive_vehicle(m.vehicle, 0.0f, 0.0f, 0.0f, 1.0f);  // left: handbrake on
                m.parked = true;
            }
            continue;
        }
        if (const auto* v = world.get<world::Vehicle>(m.entity); v && m.entity == driving_) {
            drive(world, m, *v, frame, move, dt, at);
            rotation = {0.0f, 0.0f, degrees(m.yaw)};
            moved = true;
        }
        if (!moved) continue;
        world.set_world_matrix(m.entity, trs(at, rotation, m.world_scale));
        if (m.body != physics::kWorldBody) physics_->move_kinematic(m.body, translation(at) * rotation_euler(rotation) * translation(mul(m.centre, m.scale)), dt);
    }
}

// After the physics step: a driver or rider sits on its seat, wherever the vehicle went.
void PlaySession::place_rider(world::World& world) {
    const world::Entity on = world.alive(driving_) ? driving_ : world.alive(riding_) ? riding_ : world::Entity{};
    if (!on.valid()) return;
    const auto* v = world.get<world::Vehicle>(on);
    const auto* t = world.get<world::Transport>(on);
    if (!v && !t) return;
    const Mat4 m = world.world_matrix(on);
    const bool passenger = v && on == riding_ && passenger_seat_ > 0 && passenger_seat_ <= v->passenger_seats.count;
    const Vec3 local = passenger ? v->passenger_seats[passenger_seat_ - 1] : v ? v->seat : t->seat;
    const Vec3 seat = transform_point(m, local) + Vec3{0, 0, 0.02f};
    physics_->teleport_character(seat);
    feet_ = seat;
    if (v || (t && !t->free_look)) {  // facing the way it travels
        const Vec3 x = axis_x(m);
        if (length(x) > 0.5f) yaw_ = std::atan2(x.y, x.x);
    }
}

// Driving: the camera follows behind and above the vehicle, pulled in front of walls in the way
// (Unreal's spring arm with collision test).
void PlaySession::vehicle_view(const world::World& world) {
    const world::Entity car = world.alive(driving_) ? driving_ : world.alive(riding_) && passenger_seat_ ? riding_ : world::Entity{};  // driver or passenger
    const auto* v = car.valid() ? world.get<world::Vehicle>(car) : nullptr;
    if (!v || view_camera_.valid()) return;
    const Mat4 m = world.world_matrix(car);
    const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    const Vec3 forward = axis_x(m);
    const Vec3 pivot = at + Vec3{0, 0, 1.0f};
    view_eye_ = at - forward * v->camera_distance + Vec3{0, 0, v->camera_height};
    u32 own = physics::kWorldBody;  // the vehicle's own body is not in the way
    for (const Moving& mv : moving_) own = mv.entity == car ? mv.body : own;
    const Vec3 arm = view_eye_ - pivot;
    const f32 reach = length(arm);
    physics::RayHit hit;
    if (reach > 0.01f && physics_->raycast(pivot, arm, reach, hit, own)) view_eye_ = pivot + arm * (std::max(hit.distance - 0.3f, 0.2f) / reach);
    view_forward_ = normalize(pivot - view_eye_);
}

u32 PlaySession::ride_command(world::World& world, int what, world::Entity e) {
    auto beside = [&](world::Entity of) {  // out to the left side, standing
        const Mat4 m = world.world_matrix(of);
        const Vec3 at{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        const Vec3 x = axis_x(m);
        const Vec3 left{-x.y, x.x, 0.0f};
        const f32 half_width = length(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)}) * 0.5f;
        physics_->teleport_character(at + left * (half_width + 1.0f) + Vec3{0, 0, 0.1f});
        feet_ = physics_->character_feet();
    };
    const auto replicated_of = [&](world::Entity v) -> const world::NetReplicated* { return net_.active() && world.alive(v) ? world.get<world::NetReplicated>(v) : nullptr; };
    switch (what) {
    case 0:  // enter a vehicle
        if (!world.alive(e) || !world.get<world::Vehicle>(e) || !moving_of(e)) return 0;
        if (const auto* rep = replicated_of(e)) {  // multiplayer: the server hands out the seats
            if (net_.mode() == net::Mode::Server) {
                const i32 seat = server_take_seat(world, e, net::kServerPlayer);
                if (seat >= 0) take_seat(world, e, seat);
                return seat >= 0 ? 1u : 0u;
            }
            char args[32];
            std::snprintf(args, sizeof args, "[%u]", rep->net_id);
            net_.send_event(net::kToServer, "__seat", args);  // "__seated" comes back
            return 1;
        }
        if (world.alive(riding_)) ride_command(world, 4, {});
        driving_ = e;
        scripts_.send(e, "on_enter", character_);
        return 1;
    case 1:  // exit it
        if (!world.alive(driving_)) return 0;
        if (const auto* rep = replicated_of(driving_)) {  // the seat (and the car) go back to the server
            char args[32];
            std::snprintf(args, sizeof args, "[%u]", rep->net_id);
            if (net_.mode() == net::Mode::Server) server_free_seats(world, net::kServerPlayer);
            else net_.send_event(net::kToServer, "__unseat", args);
        }
        if (Moving* m = moving_of(driving_)) m->speed = 0.0f;
        beside(driving_);
        scripts_.send(driving_, "on_exit", character_);
        driving_ = {};
        return 1;
    case 2: return world.alive(driving_) ? driving_.value : 0;  // the vehicle driven
    case 3:  // hitch a ride
        if (!world.alive(e) || !world.get<world::Transport>(e)) return 0;
        if (world.alive(driving_)) ride_command(world, 1, {});
        riding_ = e;
        scripts_.send(e, "on_enter", character_);
        return 1;
    case 4:  // get off
        if (!world.alive(riding_)) return 0;
        if (const auto* rep = passenger_seat_ ? replicated_of(riding_) : nullptr) {  // a passenger seat goes back
            char args[32];
            std::snprintf(args, sizeof args, "[%u]", rep->net_id);
            if (net_.mode() == net::Mode::Server) server_free_seats(world, net::kServerPlayer);
            else net_.send_event(net::kToServer, "__unseat", args);
        }
        beside(riding_);
        scripts_.send(riding_, "on_exit", character_);
        riding_ = {};
        passenger_seat_ = 0;
        return 1;
    case 5:
    case 6:
    case 7: {  // movers: start, stop, restart
        Moving* m = world.alive(e) && world.get<world::Mover>(e) ? moving_of(e) : nullptr;
        if (!m) return 0;
        if (what == 6) m->running = false;
        else if (what == 7 || m->started_at < 0.0) restart_mover(world, *m);
        else m->running = true;
        return 1;
    }
    default: return 0;
    }
}

} // namespace lsd::editor
