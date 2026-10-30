#include "play_session.h"

#include "lsd/foundation/log.h"
#include "lsd/world/components.h"

#include <cmath>

namespace lsd::editor {

namespace {

constexpr f32 kRadius = 0.3f;      // half width of the player's box
constexpr f32 kStepHeight = 0.4f;  // ledges up to this high are walked up, like stairs

struct Box {
    Vec3 lo, hi;
};

f32& axis(Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
f32 axis(const Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

bool overlaps(const Box& a, const Box& b) {
    return a.lo.x < b.hi.x && a.hi.x > b.lo.x && a.lo.y < b.hi.y && a.hi.y > b.lo.y && a.lo.z < b.hi.z && a.hi.z > b.lo.z;
}

// World-space box of a local box under a world matrix.
Box world_box(const Mat4& m, Vec3 lo, Vec3 hi) {
    Box box{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = transform_point(m, {(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z});
        box.lo = vmin(box.lo, p);
        box.hi = vmax(box.hi, p);
    }
    return box;
}

// Volumes: the unit cube (x, y in -0.5..0.5, z in 0..1) scaled by the entity's transform.
template <typename Volume>
u32 gather_volumes(const world::World& world, Box* out, u32 n, u32 max) {
    const reflect::TypeInfo& type = reflect::type_of<Volume>();
    for (u32 i = 0; i < world.count(type) && n < max; ++i) {
        if (!static_cast<const Volume*>(world.at(type, i))->enabled) continue;
        out[n++] = world_box(world.world_matrix(world.owner(type, i)), {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
    }
    return n;
}

// World-space bounding boxes of everything the player collides with.
// shortcut: rebuilt every frame (fine for blockout-sized levels).
u32 gather_boxes(const world::World& world, MeshCache& meshes, Box* out, u32 max) {
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    u32 n = 0;
    for (u32 i = 0; i < world.count(type) && n < max; ++i) {
        const auto* mesh = static_cast<const world::StaticMesh*>(world.at(type, i));
        if (!mesh->collision) continue;
        const CachedMesh* cached = meshes.get(mesh->mesh);
        if (!cached) continue;
        Box box = world_box(world.world_matrix(world.owner(type, i)), cached->bounds_min, cached->bounds_max);
        // Planes are flat: give them some thickness so the player can stand on them.
        if (box.hi.z - box.lo.z < 0.05f) box.lo.z -= 0.05f;
        out[n++] = box;
    }
    return gather_volumes<world::BlockingVolume>(world, out, n, max);
}

} // namespace

Vec3 PlaySession::forward() const {
    return {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_)};
}

void PlaySession::start(const world::World& world, Vec3 eye, f32 yaw, f32 pitch) {
    const reflect::TypeInfo& type = reflect::type_of<world::PlayerStart>();
    eye_height_ = 1.7f;
    position_ = eye - Vec3{0, 0, eye_height_};
    yaw_ = yaw;
    pitch_ = pitch;
    if (world.count(type) > 0) {
        const world::Entity start = world.owner(type, 0);
        const auto* settings = static_cast<const world::PlayerStart*>(world.get(start, type));
        eye_height_ = settings->eye_height;
        walk_speed_ = settings->walk_speed;
        jump_speed_ = settings->jump_speed;
        Vec3 rotation, scale;
        decompose_trs(world.world_matrix(start), position_, rotation, scale);
        yaw_ = radians(rotation.z);
        pitch_ = 0.0f;
    } else {
        LSD_LOG_INFO("editor", "No Player Start in the level: playing from the camera.");
    }
    spawn_ = position_;
    spawn_yaw_ = yaw_;
    velocity_ = {};
    on_ground_ = false;
}

void PlaySession::update(const world::World& world, MeshCache& meshes, const PlayInput& in, f32 dt) {
    dt = dt > 0.05f ? 0.05f : dt;  // a long frame (breakpoint, window drag) must not tunnel through floors
    yaw_ -= in.look_x * 0.0025f;
    pitch_ -= in.look_y * 0.0025f;
    pitch_ = pitch_ < -1.5f ? -1.5f : pitch_ > 1.5f ? 1.5f : pitch_;

    const Vec3 ahead{std::cos(yaw_), std::sin(yaw_), 0};
    const Vec3 right{std::sin(yaw_), -std::cos(yaw_), 0};
    Vec3 wish = ahead * static_cast<f32>(in.forward - in.back) + right * static_cast<f32>(in.right - in.left);
    if (length(wish) > 0.0f) wish = normalize(wish) * (walk_speed_ * (in.sprint ? 1.8f : 1.0f));
    velocity_.x = wish.x;
    velocity_.y = wish.y;
    velocity_.z -= world.settings().gravity * dt;
    if (in.jump && on_ground_) velocity_.z = jump_speed_;

    static Box boxes[4096];
    const u32 count = gather_boxes(world, meshes, boxes, 4096);
    const f32 height = eye_height_ + 0.1f;
    auto player_box = [&] { return Box{position_ - Vec3{kRadius, kRadius, 0}, position_ + Vec3{kRadius, kRadius, height}}; };

    // Move one axis at a time and push out of whatever it hits (simple and stable for boxes).
    const bool was_on_ground = on_ground_;
    on_ground_ = false;
    for (int a = 0; a < 3; ++a) {
        const f32 move = axis(velocity_, a) * dt;
        if (move == 0.0f) continue;
        axis(position_, a) += move;
        for (u32 i = 0; i < count; ++i) {
            const Box me = player_box();
            if (!overlaps(me, boxes[i])) continue;
            if (a < 2 && was_on_ground && boxes[i].hi.z - position_.z <= kStepHeight) {
                const f32 old_z = position_.z;
                position_.z = boxes[i].hi.z;  // step up onto it if there is room
                bool blocked = false;
                for (u32 j = 0; j < count && !blocked; ++j) blocked = overlaps(player_box(), boxes[j]);
                if (!blocked) continue;
                position_.z = old_z;
            }
            const f32 lo = axis(me.lo, a) - axis(position_, a), hi = axis(me.hi, a) - axis(position_, a);  // player extents
            axis(position_, a) = move > 0 ? axis(boxes[i].lo, a) - hi : axis(boxes[i].hi, a) - lo;
            if (a == 2) {
                if (move < 0) on_ground_ = true;
                velocity_.z = 0.0f;
            }
        }
    }

    static Box kill[256];
    const u32 kill_count = gather_volumes<world::KillVolume>(world, kill, 0, 256);
    bool killed = position_.z < world.settings().kill_z;
    for (u32 i = 0; i < kill_count && !killed; ++i) killed = overlaps(player_box(), kill[i]);
    if (killed) {
        position_ = spawn_;
        yaw_ = spawn_yaw_;
        velocity_ = {};
    }
}

} // namespace lsd::editor
