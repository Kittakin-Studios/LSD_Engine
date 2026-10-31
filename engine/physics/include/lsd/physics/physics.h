#pragma once
// Physics on Jolt (hidden behind this header): rigid bodies and one character controller.
// World convention as everywhere: Z up, metres, seconds.
#include "lsd/foundation/math.h"

namespace lsd::physics {

enum class Shape : u8 { Box, Sphere, Capsule };
enum class Motion : u8 { Static, Dynamic, Kinematic };

struct BodyDesc {
    Shape shape = Shape::Box;
    Vec3 half_extents{0.5f, 0.5f, 0.5f};  // Box; Sphere uses x as the radius; Capsule: x = radius, z = half height of the cylinder (along Z)
    Mat4 transform;                         // position + rotation of the shape's centre (no scale: put it in half_extents)
    Motion motion = Motion::Static;
    f32 mass = 10.0f;  // kg, dynamic only
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
};

struct CharacterDesc {
    Vec3 feet;
    f32 radius = 0.35f;
    f32 height = 1.8f;           // total, feet to top of head
    f32 max_slope_degrees = 45.0f;
    f32 step_height = 0.4f;      // stairs up to this high are walked up
};

constexpr u32 kCharacterBody = 0xFFFFFFFFu;  // stands for the player in contacts

// A shape for sweeps and overlaps.
struct QueryShape {
    enum Kind : u8 { Sphere, Box, Capsule };
    Kind kind = Sphere;
    f32 radius = 0.5f;                     // sphere, capsule
    f32 half_height = 0.5f;                // capsule: half the straight part between its caps
    Vec3 half_extents{0.5f, 0.5f, 0.5f};   // box
};

struct RayHit {
    u32 body = 0;      // index from add_body
    Vec3 point;
    Vec3 normal;
    f32 distance = 0;
};

// Two things started touching during the last step (b may be kCharacterBody).
struct Contact {
    u32 a, b;
};

enum class JointType : u8 { Fixed, Hinge, Ball, Slider };
constexpr u32 kWorldBody = 0xFFFFFFFEu;  // a joint's second body: the static world

// Holds two bodies together (Unreal's Physics Constraint). Pivot and axis are in world space at
// creation; limits are degrees for a hinge, metres for a slider (from where it starts).
struct JointDesc {
    JointType type = JointType::Hinge;
    u32 body_a = 0;
    u32 body_b = kWorldBody;
    Vec3 pivot;
    Vec3 axis{0, 0, 1};
    bool limited = false;
    f32 limit_min = -45.0f;
    f32 limit_max = 45.0f;
    f32 motor_speed = 0.0f;  // hinge deg/s or slider m/s; 0 = no motor
    f32 motor_strength = 1000.0f;  // N m (hinge) or N (slider)
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Returns the body's index (0, 1, 2... in the order added).
    u32 add_body(const BodyDesc& desc);
    // Moves a Kinematic body to `transform` over the next step (it pushes and carries what it meets).
    void move_kinematic(u32 body, const Mat4& transform, f32 dt);
    // Takes a body out of the simulation; its index is not reused.
    void remove_body(u32 body);
    // Position + rotation of a body's centre after the last step (identity once removed).
    Mat4 body_transform(u32 body) const;
    u32 body_count() const;

    // Nearest body along the ray (direction need not be unit length; the player is not hit).
    bool raycast(Vec3 origin, Vec3 direction, f32 max_distance, RayHit& hit) const;
    // The first body a shape touches moving from `from` to `to` (Unreal's Sphere / Box / Capsule Trace);
    // hit.distance is how far the shape's centre got. Boxes stay axis-aligned, capsules stand along Z.
    bool sweep(const QueryShape& shape, Vec3 from, Vec3 to, RayHit& hit) const;
    // Bodies the shape overlaps when placed at `at` (Unreal's Sphere / Box Overlap Actors). Returns how many were written.
    u32 overlap(const QueryShape& shape, Vec3 at, u32* bodies, u32 capacity) const;
    // Contacts that began since the previous call (bodies with each other and with the player).
    u32 take_contacts(Contact* out, u32 capacity);

    // Returns the joint's index; joints on a removed body go with it.
    u32 add_joint(const JointDesc& desc);
    u32 joint_count() const;

    void set_gravity(f32 metres_per_second2);
    void step(f32 dt);

    // The player. Velocity is horizontal (x, y); jump starts a jump if on the ground.
    void create_character(const CharacterDesc& desc);
    // `swimming`: no gravity and no ground; velocity.z moves up and down (water).
    void move_character(Vec3 velocity, bool jump, f32 jump_speed, f32 dt, bool swimming = false);
    // Changes the capsule's height, feet staying put (crouching). False, and unchanged, if it would not fit (a ceiling).
    bool set_character_height(f32 height);
    void teleport_character(Vec3 feet);
    Vec3 character_feet() const;
    bool character_on_ground() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace lsd::physics
