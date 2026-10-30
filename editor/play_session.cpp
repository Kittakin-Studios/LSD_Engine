#include "play_session.h"

#include "graph_editor.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/world/prefab.h"
#include "lsd/world/spline.h"

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

} // namespace

PlaySession::~PlaySession() { stop(); }

void PlaySession::stop() {
    scripts_.stop();
    delete physics_;
    physics_ = nullptr;
    simulated_count_ = 0;
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
    return false;
}

void PlaySession::start(world::World& world, MeshCache& meshes, const asset::AssetRegistry& registry, Vec3 eye, f32 yaw, f32 pitch) {
    stop();
    registry_ = &registry;
    world_ = &world;
    meshes_ = &meshes;
    physics_ = new physics::PhysicsWorld();
    physics_->set_gravity(world.settings().gravity);
    start_game_mode(world);  // may spawn the player and a GameMode object first

    // Level geometry is static; RigidBody objects are simulated.
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        if (const world::Entity e = world.entity_at_slot(slot); e.valid()) add_entity_bodies(world, e);
    }
    add_joints(world, {});
    Vec3 scale;

    // The player: the first Character, else a default walker at the Player Start or the camera.
    const reflect::TypeInfo& character_type = reflect::type_of<world::Character>();
    const reflect::TypeInfo& start_type = reflect::type_of<world::PlayerStart>();
    settings_ = world::Character{};
    character_ = {};
    wired_ = false;
    Vec3 rotation;
    if (world.count(character_type) > 0) {
        character_ = world.owner(character_type, 0);
        settings_ = *world.get<world::Character>(character_);
        decompose_trs(world.world_matrix(character_), feet_, rotation, scale);
        yaw_ = radians(rotation.z);
        pitch_ = 0.0f;
        input::InputMappingContext context;
        if (load_asset(registry, settings_.mapping_context, context)) {
            input_.add_context(context);
            for (const input::KeyMapping& m : context.mappings) {  // each action's trigger (Hold, Tap...) from its asset
                input::InputAction action;
                if (load_asset(registry, m.action, action)) input_.set_trigger(m.action, static_cast<input::Trigger>(action.trigger), action.trigger_time);
            }
            wired_ = true;
        } else {
            LSD_LOG_WARN("play", "%s has no Input Mapping Context (pick one in its Details): it cannot move", world.name(character_));
        }
        if (!settings_.move_action[0] || !settings_.look_action[0] || !settings_.jump_action[0]) {
            LSD_LOG_WARN("play", "%s: pick its Move, Look and Jump actions in Details to control it", world.name(character_));
        }
    } else {
        input_.add_context(default_controls());
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

    script::ScriptHost host;
    host.user = this;
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
    host.table_path = [](void* user, const char* name, char* out, usize capacity) {
        const auto& self = *static_cast<const PlaySession*>(user);
        const asset::AssetRecord* r = self.find_asset(asset::AssetType::Table, name);
        if (r) self.registry_->source_path(*r, out, capacity);
        return r != nullptr;
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
    if (const auto* rigid = world.get<world::RigidBody>(e)) {
        if (simulated_count_ == sizeof simulated_ / sizeof simulated_[0]) return;
        physics::BodyDesc d = fitted_body(world.world_matrix(e), cached ? cached->bounds_min : kUnitLo, cached ? cached->bounds_max : kUnitHi,
                                          rigid->shape, physics::Motion::Dynamic, scale, centre);
        d.mass = rigid->mass;
        d.friction = rigid->friction;
        d.restitution = rigid->bounciness;
        const u32 body = add_body(e, d);
        simulated_[simulated_count_++] = {e, body, scale, centre};
        return;
    }
    if (cached && mesh->collision) {
        add_body(e, fitted_body(world.world_matrix(e), cached->bounds_min, cached->bounds_max, 0, physics::Motion::Static, scale, centre));
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
    if (!physics_) return;
    dt = dt > 0.05f ? 0.05f : dt;  // a long frame (breakpoint, window drag) must not tunnel through floors
    input_.update(frame, dt);
    const input::Value move = input_.value(settings_.move_action);
    const input::Value look = input_.value(settings_.look_action);
    const bool jump = input_.fired(settings_.jump_action);  // its trigger: Pressed by default, or Hold, Tap...

    yaw_ -= radians(look.x * settings_.look_sensitivity);
    pitch_ += radians(look.y * settings_.look_sensitivity) * (settings_.invert_look_y ? -1.0f : 1.0f);
    pitch_ = pitch_ < -1.5f ? -1.5f : pitch_ > 1.5f ? 1.5f : pitch_;
    const Vec3 ahead{std::cos(yaw_), std::sin(yaw_), 0};
    const Vec3 right{std::sin(yaw_), -std::cos(yaw_), 0};
    Vec3 velocity = ahead * move.y + right * move.x;
    const f32 amount = length(velocity);
    if (amount > 1.0f) velocity = velocity * (1.0f / amount);
    update_followers(world, dt);  // before the step: moving platforms carry the player and objects
    physics_->move_character(velocity * settings_.walk_speed, jump, settings_.jump_speed, dt);
    physics_->step(dt);
    feet_ = physics_->character_feet();
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
        if (!world.alive(s.entity)) continue;  // a script destroyed it
        world.set_world_matrix(s.entity, physics_->body_transform(s.body) * translation(-mul(s.centre, s.scale)) * scaling(s.scale));
    }
    if (world.alive(character_)) {
        Vec3 t, r, scale;
        decompose_trs(world.world_matrix(character_), t, r, scale);
        world.set_world_matrix(character_, trs(feet_, {0, 0, degrees(yaw_)}, scale));
    }
    // Camera: the head, or behind it on the spring arm, pulled in when a wall is in the way.
    const Vec3 head = feet_ + Vec3{0, 0, settings_.eye_height};
    eye_ = head;
    if (settings_.camera_distance > 0.0f) {
        const Vec3 back = forward() * -1.0f;
        f32 arm = settings_.camera_distance;
        physics::RayHit hit;
        if (physics_->raycast(head, back, arm, hit)) arm = hit.distance > 0.2f ? hit.distance - 0.2f : 0.0f;
        eye_ = head + back * arm;
    }
    send_hits();
    update_triggers(world);
    scripts_.update(dt);
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
        scripts_.send(entered[i].trigger, "on_overlap_begin", entered[i].other);
        scripts_.send(entered[i].other, "on_overlap_begin", entered[i].trigger);
    }
}

} // namespace lsd::editor
