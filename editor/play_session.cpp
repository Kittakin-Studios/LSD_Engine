#include "play_session.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

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

bool load_context(const asset::AssetRegistry& registry, const char* guid_text, input::InputMappingContext& out) {
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
    input_.clear();
}

Vec3 PlaySession::forward() const {
    return {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_)};
}

bool PlaySession::script_path(void* user, const char* guid_text, char* out, usize capacity) {
    const auto& registry = *static_cast<const asset::AssetRegistry*>(user);
    asset::Guid guid;
    const asset::AssetRecord* r = asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
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
    physics_ = new physics::PhysicsWorld();
    physics_->set_gravity(world.settings().gravity);

    // Level geometry: meshes with collision (unless simulated) and Blocking Volumes are static.
    Vec3 scale, centre;
    const reflect::TypeInfo& mesh_type = reflect::type_of<world::StaticMesh>();
    for (u32 i = 0; i < world.count(mesh_type); ++i) {
        const world::Entity e = world.owner(mesh_type, i);
        const auto* mesh = static_cast<const world::StaticMesh*>(world.at(mesh_type, i));
        const CachedMesh* cached = meshes.get(mesh->mesh);
        if (!mesh->collision || !cached || world.get<world::RigidBody>(e)) continue;
        physics_->add_body(fitted_body(world.world_matrix(e), cached->bounds_min, cached->bounds_max, 0, physics::Motion::Static, scale, centre));
    }
    const reflect::TypeInfo& blocking = reflect::type_of<world::BlockingVolume>();
    for (u32 i = 0; i < world.count(blocking); ++i) {
        if (!static_cast<const world::BlockingVolume*>(world.at(blocking, i))->enabled) continue;
        physics_->add_body(fitted_body(world.world_matrix(world.owner(blocking, i)), kUnitLo, kUnitHi, 0, physics::Motion::Static, scale, centre));
    }
    // Simulated objects.
    const reflect::TypeInfo& rigid_type = reflect::type_of<world::RigidBody>();
    for (u32 i = 0; i < world.count(rigid_type) && simulated_count_ < sizeof simulated_ / sizeof simulated_[0]; ++i) {
        const world::Entity e = world.owner(rigid_type, i);
        const auto* rigid = static_cast<const world::RigidBody*>(world.at(rigid_type, i));
        Vec3 lo = kUnitLo, hi = kUnitHi;
        if (const auto* mesh = world.get<world::StaticMesh>(e)) {
            if (const CachedMesh* cached = meshes.get(mesh->mesh)) {
                lo = cached->bounds_min;
                hi = cached->bounds_max;
            }
        }
        physics::BodyDesc d = fitted_body(world.world_matrix(e), lo, hi, rigid->shape, physics::Motion::Dynamic, scale, centre);
        d.mass = rigid->mass;
        d.friction = rigid->friction;
        d.restitution = rigid->bounciness;
        simulated_[simulated_count_++] = {e, physics_->add_body(d), scale, centre};
    }

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
        if (load_context(registry, settings_.mapping_context, context)) {
            input_.add_context(context);
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
        return action_guid(*self.registry_, action, guid) && self.input_.pressed(guid);
    };
    scripts_.start(world, host, script_path, const_cast<asset::AssetRegistry*>(&registry));
}

void PlaySession::update(world::World& world, const input::InputFrame& frame, f32 dt) {
    if (!physics_) return;
    dt = dt > 0.05f ? 0.05f : dt;  // a long frame (breakpoint, window drag) must not tunnel through floors
    input_.update(frame);
    const input::Value move = input_.value(settings_.move_action);
    const input::Value look = input_.value(settings_.look_action);
    const bool jump = input_.pressed(settings_.jump_action);

    yaw_ -= radians(look.x * settings_.look_sensitivity);
    pitch_ += radians(look.y * settings_.look_sensitivity) * (settings_.invert_look_y ? -1.0f : 1.0f);
    pitch_ = pitch_ < -1.5f ? -1.5f : pitch_ > 1.5f ? 1.5f : pitch_;
    const Vec3 ahead{std::cos(yaw_), std::sin(yaw_), 0};
    const Vec3 right{std::sin(yaw_), -std::cos(yaw_), 0};
    Vec3 velocity = ahead * move.y + right * move.x;
    const f32 amount = length(velocity);
    if (amount > 1.0f) velocity = velocity * (1.0f / amount);
    physics_->move_character(velocity * settings_.walk_speed, jump, settings_.jump_speed, dt);
    physics_->step(dt);
    feet_ = physics_->character_feet();

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
