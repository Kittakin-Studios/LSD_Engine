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

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"
#include "lsd/world/timeline.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

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
    if (ui_) ui_->close_all();
    ui_cursor_ = false;
    ui_follower_count_ = 0;
    scripts_.stop();
    nav_.clear();
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

void PlaySession::apply_input() {
    input_.clear();
    input_.add_context(context_);
    for (const input::KeyMapping& m : context_.mappings) {  // each action's trigger (Hold, Tap...) from its asset
        input::InputAction action;
        if (registry_ && load_asset(*registry_, m.action, action)) input_.set_trigger(m.action, static_cast<input::Trigger>(action.trigger), action.trigger_time);
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
    if (!travel) scripts_.clear_persistent();  // a new Play: a fresh game instance
    level_request_[0] = '\0';
    registry_ = &registry;
    world_ = &world;
    meshes_ = &meshes;
    physics_ = new physics::PhysicsWorld();
    physics_->set_gravity(world.settings().gravity);
    start_game_mode(world);  // may spawn the player and a GameMode object first
    if (world.settings().level_script[0]) {  // the Level Blueprint: a script on an object of its own
        std::snprintf(world.add<world::Script>(world.create("LevelScript"))->script, sizeof(world::Script::script), "%s", world.settings().level_script);
    }

    // Level geometry is static; RigidBody objects are simulated.
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        if (const world::Entity e = world.entity_at_slot(slot); e.valid()) add_entity_bodies(world, e);
    }
    add_joints(world, {});
    agent_count_ = 0;
    if (build_level_navmesh(world, *meshes_, nav_)) LSD_LOG_INFO("play", "Navmesh built");
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
    if (user_settings_) {  // the player's rebound keys
        for (const KeyBinding& b : user_settings_->key_bindings) {
            char guid[33];
            if (!action_guid(registry, b.action, guid)) continue;
            for (input::KeyMapping& m : context_.mappings) {
                if (std::strcmp(m.action, guid) == 0 && SDL_strcasecmp(m.key, b.from) == 0) std::snprintf(m.key, sizeof m.key, "%s", b.to);
            }
        }
    }
    apply_input();
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
    host.set_anim_param = [](void* user, world::Entity e, const char* name, f32 value) {
        auto& self = *static_cast<PlaySession*>(user);
        if (self.animations_) self.animations_->set_param(*self.world_, e, name, value);
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
        const physics::Motion motion = rigid->kinematic ? physics::Motion::Kinematic : physics::Motion::Dynamic;
        physics::BodyDesc d = fitted_body(world.world_matrix(e), cached ? cached->bounds_min : kUnitLo, cached ? cached->bounds_max : kUnitHi,
                                          rigid->shape == 3 ? 0 : rigid->shape, motion, scale, centre);
        if (rigid->shape == 3) use_mesh_shape(d, physics::Shape::Convex);
        d.mass = rigid->mass;
        d.friction = rigid->friction;
        d.restitution = rigid->bounciness;
        d.debris = rigid->collision_preset == 1;
        apply_material(d, rigid->physical_material);
        const u32 body = add_body(e, d);
        simulated_[simulated_count_++] = {e, body, scale, centre, rigid->kinematic};
        return;
    }
    if (cached && mesh->collision) {
        physics::BodyDesc d = fitted_body(world.world_matrix(e), cached->bounds_min, cached->bounds_max, 0, physics::Motion::Static, scale, centre);
        if (mesh->collision_shape == 1) use_mesh_shape(d, physics::Shape::Mesh);
        apply_material(d, mesh->physical_material);
        add_body(e, d);
    }
    if (const auto* blocking = world.get<world::BlockingVolume>(e); blocking && blocking->enabled) {
        add_body(e, fitted_body(world.world_matrix(e), kUnitLo, kUnitHi, 0, physics::Motion::Static, scale, centre));
    }
}

// A prefab, mesh asset or built-in shape by name, placed at `position` (world objects only).
world::Entity PlaySession::create_spawn(world::World& world, const char* name, Vec3 position) {
    const asset::AssetRecord* prefab = find_asset(asset::AssetType::Prefab, name);
    const asset::AssetRecord* mesh_asset = prefab ? nullptr : find_asset(asset::AssetType::Mesh, name);
    char key[40];
    if (mesh_asset) asset::to_string(mesh_asset->guid, key);
    else std::snprintf(key, sizeof key, "%s", name);  // built-in shapes: Cube, Sphere...
    if (!prefab && !meshes_->get(key)) return {};

    char unique[64];
    world.unique_name(name, unique, sizeof unique);
    const world::Entity e = world.create(unique);
    world.get<world::Transform>(e)->position = position;
    if (prefab) {
        asset::to_string(prefab->guid, world.add<world::PrefabInstance>(e)->prefab);
        char path[300];
        registry_->source_path(*prefab, path, sizeof path);
        world::update_instance_from_file(world, e, path, true);
    } else {
        std::snprintf(world.add<world::StaticMesh>(e)->mesh, sizeof(world::StaticMesh::mesh), "%s", key);
    }
    return e;
}

bool PlaySession::take_level_request(char* path, usize capacity) {
    if (!level_request_[0]) return false;
    std::snprintf(path, capacity, "%s", level_request_);
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
        physics_->add_joint(d);
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
        Vec3 points[32];
        const Mat4 m = world.world_matrix(f.spline);
        for (u32 k = 0; k < spline->points.count; ++k) points[k] = transform_point(m, spline->points[k]);
        const f32 total = world::spline_length(points, spline->points.count, spline->closed);
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
        const Vec3 at = world::spline_at_distance(points, spline->points.count, spline->closed, f.distance, &tangent);
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
// volume's walls are muffled.
void PlaySession::update_audio_volumes(const world::World& world, f32 dt) {
    if (!sounds_) return;
    audio::AudioSystem& a = *sounds_->audio();
    const reflect::TypeInfo& type = reflect::type_of<world::AudioVolume>();
    auto volume_at = [&](Vec3 p) {
        for (u32 i = 0; i < world.count(type); ++i) {
            const auto* v = static_cast<const world::AudioVolume*>(world.at(type, i));
            if (v->enabled && inside_volume(world, world.owner(type, i), p)) return v;
        }
        return static_cast<const world::AudioVolume*>(nullptr);
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
        a.set_voice_lowpass(v.id, wall ? wall->muffle : 20000.0f);
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

void PlaySession::update(world::World& world, const input::InputFrame& frame, f32 dt) {
    LSD_PROFILE_SCOPE("play");
    if (!physics_) return;
    dt = dt > 0.05f ? 0.05f : dt;  // a long frame (breakpoint, window drag) must not tunnel through floors
    input_.update(frame, dt);
    char held[32] = "";
    input::held_key(frame, held, sizeof held);
    std::snprintf(key_pressed_, sizeof key_pressed_, "%s", held[0] && std::strcmp(held, key_held_) != 0 ? held : "");
    std::snprintf(key_held_, sizeof key_held_, "%s", held);
    const input::Value move = input_.value(settings_.move_action);
    const input::Value look = input_.value(settings_.look_action);
    const bool jump = input_.fired(settings_.jump_action);  // its trigger: Pressed by default, or Hold, Tap...

    const f32 sensitivity = settings_.look_sensitivity * (user_settings_ ? user_settings_->mouse_sensitivity : 1.0f);
    const bool invert = settings_.invert_look_y != (user_settings_ && user_settings_->invert_look);
    yaw_ -= radians(look.x * sensitivity);
    pitch_ += radians(look.y * sensitivity) * (invert ? -1.0f : 1.0f);
    pitch_ = pitch_ < -1.5f ? -1.5f : pitch_ > 1.5f ? 1.5f : pitch_;
    const Vec3 ahead{std::cos(yaw_), std::sin(yaw_), 0};
    const Vec3 right{std::sin(yaw_), -std::cos(yaw_), 0};
    Vec3 velocity = ahead * move.y + right * move.x;
    const f32 amount = length(velocity);
    if (amount > 1.0f) velocity = velocity * (1.0f / amount);
    update_followers(world, dt);  // before the step: moving platforms carry the player and objects
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
        const f32 n = length(swim);
        if (n > 1.0f) swim = swim * (1.0f / n);
        physics_->move_character(swim * settings_.swim_speed, false, 0.0f, dt, true);
    } else {
        physics_->move_character(velocity * (crouched_ ? settings_.crouch_speed : settings_.walk_speed), jump && !crouched_, settings_.jump_speed, dt);
    }
    physics_->step(dt);
    const Vec3 moved = physics_->character_feet() - feet_;
    feet_ = physics_->character_feet();
    if (animations_ && character_.valid() && dt > 0.0f) {
        animations_->set_param(world, character_, "Speed", std::sqrt(moved.x * moved.x + moved.y * moved.y) / dt);
        animations_->set_param(world, character_, "VerticalSpeed", moved.z / dt);
        animations_->set_param(world, character_, "InAir", physics_->character_on_ground() || swimming_ ? 0.0f : 1.0f);
        animations_->set_param(world, character_, "Crouching", crouched_ ? 1.0f : 0.0f);
        animations_->set_param(world, character_, "Swimming", swimming_ ? 1.0f : 0.0f);
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
    if (world.alive(character_)) {
        Vec3 t, r, scale;
        decompose_trs(world.world_matrix(character_), t, r, scale);
        world.set_world_matrix(character_, trs(feet_, {0, 0, degrees(yaw_)}, scale));
    }
    // Camera: the head, or behind it on the spring arm, pulled in when a wall is in the way.
    const Vec3 head = feet_ + Vec3{0, 0, settings_.eye_height * (crouched_ ? settings_.crouch_height / settings_.height : 1.0f)};
    eye_ = head;
    if (settings_.camera_distance > 0.0f) {
        const Vec3 back = forward() * -1.0f;
        f32 arm = settings_.camera_distance;
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
        scripts_.update(dt);
    }
    update_brains(world, dt);
    update_agents(world, dt);
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
    view_forward_ = forward();
    view_fov_ = 0.0f;
    const reflect::TypeInfo& camera_type = reflect::type_of<world::Camera>();
    world::Entity camera = view_camera_;
    if (!world.alive(camera) || !world.get<world::Camera>(camera)) {
        camera = {};
        for (u32 i = 0; i < world.count(camera_type) && !camera.valid(); ++i) {
            if (static_cast<const world::Camera*>(world.at(camera_type, i))->auto_activate) camera = world.owner(camera_type, i);
        }
    }
    if (!camera.valid()) return;
    const Mat4 m = world.world_matrix(camera);
    view_eye_ = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    view_forward_ = normalize(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
    view_fov_ = radians(world.get<world::Camera>(camera)->fov);
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

} // namespace lsd::editor
