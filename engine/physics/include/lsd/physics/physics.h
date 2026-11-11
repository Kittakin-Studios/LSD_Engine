#pragma once
// Physics on Jolt (hidden behind this header): rigid bodies and one character controller.
// World convention as everywhere: Z up, metres, seconds.
#include "lsd/foundation/math.h"

namespace lsd::physics {

// Mesh: the triangles themselves (static or kinematic only; Unreal's complex collision). Convex: the
// tightest convex hull around the points (moving bodies too).
enum class Shape : u8 { Box, Sphere, Capsule, Mesh, Convex };
enum class Motion : u8 { Static, Dynamic, Kinematic };

struct BodyDesc {
    Shape shape = Shape::Box;
    Vec3 half_extents{0.5f, 0.5f, 0.5f};  // Box; Sphere uses x as the radius; Capsule: x = radius, z = half height of the cylinder (along Z)
    Mat4 transform;                         // position + rotation of the shape's centre (no scale: put it in half_extents)
    Motion motion = Motion::Static;
    f32 mass = 10.0f;  // kg, dynamic only
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    // Mesh / Convex: points in the body's space (scale already applied); Mesh also takes triangles.
    const Vec3* points = nullptr;
    u32 point_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
    bool debris = false;  // collides with static things only: not the player, not other moving bodies
    bool one_way = false; // the player passes up (and sideways) through it and lands on its top (2D jump-through platforms)
    bool plane_2d = false;  // moves only in X-Z, turns only around Y (2D games)
    Vec3 centre_of_mass_offset;  // dynamic: moves the centre of mass (body space), e.g. down for a car that should not tip
    u32 group = 0;  // bodies sharing a non-zero group never collide with each other (a ragdoll's parts)
};

// A wheel of a physics vehicle (Unreal's Chaos Vehicle Wheel): a ray-cast suspension with a tyre.
struct WheelDesc {
    Vec3 position;                 // where its suspension is attached, in the vehicle body's space (from the body's centre)
    f32 radius = 0.35f;
    f32 width = 0.25f;
    f32 suspension_length = 0.3f;  // travel from fully up to fully down
    f32 suspension_frequency = 1.5f;  // Hz: how stiff (higher bounces faster)
    f32 suspension_damping = 0.5f;    // 0 bounces forever, 1 settles without overshoot
    f32 max_steer_degrees = 0.0f;     // 0: does not steer
    bool driven = true;               // the engine turns it
    bool handbrake = false;           // the handbrake locks it (usually the rear wheels)
    f32 friction = 1.0f;              // tyre grip, times Jolt's road curve (0.3 ice, 1 tarmac, 1.5 slicks)
};
constexpr u32 kMaxWheels = 8;

// A wheeled vehicle on a dynamic body: engine, automatic gearbox, differentials, brakes, handbrake
// (Jolt's wheeled vehicle controller; Unreal's Chaos Wheeled Vehicle Movement). Forward is the body's +X, up +Z.
struct VehicleDesc {
    u32 body = 0;  // a Dynamic body from add_body
    WheelDesc wheels[kMaxWheels];
    u32 wheel_count = 0;
    f32 engine_torque = 500.0f;    // N m at full throttle
    f32 max_rpm = 6000.0f;
    f32 brake_torque = 1500.0f;    // N m per wheel
    f32 handbrake_torque = 4000.0f;
    f32 max_pitch_roll_degrees = 60.0f;  // tilts further than this are held back (stays on its wheels)
    f32 gear_ratios[8] = {};  // forward gears, first gear first; gear_count 0: Jolt's five
    u32 gear_count = 0;
    f32 reverse_ratio = 0.0f;  // > 0: the reverse gear's ratio (0: Jolt's)
};

// A wheel fitted to its mesh or bone (Unreal's wheel bones): `lo` / `hi` are the wheel's box in the
// vehicle's own space (x forward, y left, z up); its centre, radius (half the larger of its length and
// height) and width (across).
inline void fit_wheel(Vec3 lo, Vec3 hi, Vec3& centre, f32& radius, f32& width) {
    centre = (lo + hi) * 0.5f;
    radius = 0.5f * (hi.x - lo.x > hi.z - lo.z ? hi.x - lo.x : hi.z - lo.z);
    width = hi.y - lo.y;
}

struct VehicleState {
    f32 speed = 0.0f;  // m/s along its forward axis (negative backwards)
    f32 rpm = 0.0f;
    i32 gear = 0;      // -1 reverse, 0 neutral, 1.. forward
    u32 wheels_on_ground = 0;
};

// Cloth (Unreal's Chaos Cloth): particles joined along the triangles' edges (a Jolt soft body) that
// stretch, shear and bend a little, fall, and collide with the world's bodies. Pinned particles stay put.
struct ClothDesc {
    const Vec3* points = nullptr;  // world space, one per particle (corners shared by triangles once)
    u32 point_count = 0;
    const u32* indices = nullptr;  // triangles over the points
    u32 index_count = 0;
    const u8* pinned = nullptr;    // per point, non-zero: held where it is (nullptr: none)
    f32 mass = 1.0f;               // kg, the whole cloth
    f32 stiffness = 0.8f;          // 0 stretchy and limp, 1 as stiff as it gets
    f32 damping = 0.3f;            // how quickly its motion dies down
    f32 gravity_scale = 1.0f;
    f32 thickness = 0.02f;         // m: how far it keeps from what it lies on
    u32 iterations = 5;            // solver passes per step (more: stiffer, slower)
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

// Cone: a ball joint whose swing stays inside a cone and whose twist is limited (ragdoll shoulders, hanging lamps).
enum class JointType : u8 { Fixed, Hinge, Ball, Slider, Cone };
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
    f32 spring_frequency = 0.0f;   // Hz: the limits give like a spring (hinge, slider); 0 = hard stops
    f32 spring_damping = 0.5f;     // 0 bounces forever, 1 settles without overshoot
    f32 cone_angle = 45.0f;        // Cone: how far it may swing from the axis (degrees); limit_min / max limit its twist
    f32 break_force = 0.0f;        // N: the joint snaps when pulled harder (Unreal's breakable constraint); 0 = never
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
    // Places a body at `transform` at once (a teleport: no sweep, nothing pushed). For static bodies a script moved.
    void set_body_transform(u32 body, const Mat4& transform);
    // Takes a body out of the simulation; its index is not reused.
    void remove_body(u32 body);
    // Position + rotation of a body's centre after the last step (identity once removed).
    Mat4 body_transform(u32 body) const;
    // Linear velocity of a body's centre, metres per second (zero for static or removed bodies).
    Vec3 body_velocity(u32 body) const;
    u32 body_count() const;

    // Nearest body along the ray (direction need not be unit length; the player is not hit).
    // `ignore_body`: one body the ray passes through (a vehicle looking for the ground under itself).
    bool raycast(Vec3 origin, Vec3 direction, f32 max_distance, RayHit& hit, u32 ignore_body = 0xFFFFFFFFu) const;
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
    // Joints that snapped (break_force) since the last call, by add_joint index.
    u32 take_broken_joints(u32* out, u32 capacity);

    // Returns the vehicle's index; it goes with its body (remove_body).
    u32 add_vehicle(const VehicleDesc& desc);
    // Driver input, held until changed: forward -1..1 (reverse when negative), right -1..1 (steering),
    // brake and handbrake 0..1; `torque_scale` multiplies the engine (a boost).
    void drive_vehicle(u32 vehicle, f32 forward, f32 right, f32 brake, f32 handbrake, f32 torque_scale = 1.0f);
    // A wheel's world transform after the last step (spinning, steered, on its suspension): x forward,
    // y left (its axle), z up; identity if there is no such wheel.
    Mat4 wheel_transform(u32 vehicle, u32 wheel) const;
    VehicleState vehicle_state(u32 vehicle) const;

    // Cloth: returns its body index (remove_body takes it out again).
    u32 add_cloth(const ClothDesc& desc);
    // The air around a cloth for the next steps (m/s): it pushes on each triangle facing it.
    void set_cloth_wind(u32 body, Vec3 wind);
    // A cloth's particles in world space after the last step, in add_cloth's order; how many were written.
    u32 cloth_points(u32 body, Vec3* out, u32 capacity) const;

    void set_gravity(f32 metres_per_second2);
    void step(f32 dt);

    // The player. Velocity is horizontal (x, y); jump starts a jump if on the ground.
    void create_character(const CharacterDesc& desc);
    // `swimming`: no gravity and no ground; velocity.z moves up and down (water).
    void move_character(Vec3 velocity, bool jump, f32 jump_speed, f32 dt, bool swimming = false);
    // Changes the capsule's height, feet staying put (crouching). False, and unchanged, if it would not fit (a ceiling).
    bool set_character_height(f32 height);
    // Live edits: the capsule's radius (height kept), steepest walkable slope and stair step.
    void set_character_limits(f32 radius, f32 max_slope_degrees, f32 step_height);
    // Live edits of a body: friction / bounciness, and its mass (dynamic bodies).
    void set_body_material(u32 body, f32 friction, f32 restitution);
    void set_body_mass(u32 body, f32 mass);
    // Water for the next step: the part of a dynamic body's shape below the plane through `surface`
    // (facing `normal`) pushes it up, `buoyancy` times its weight's worth (1 neither floats nor sinks),
    // and drag (about 0.5 linear, 0.05 angular) slows it there. False when it is not in the water.
    bool apply_buoyancy(u32 body, Vec3 surface, Vec3 normal, f32 buoyancy, f32 linear_drag, f32 angular_drag, f32 dt);
    // A dynamic body made kinematic (another machine simulates it: it follows move_kinematic) or
    // dynamic again; a vehicle on it stops / starts driving it.
    void set_body_kinematic(u32 body, bool kinematic);
    void teleport_character(Vec3 feet);
    Vec3 character_feet() const;
    bool character_on_ground() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace lsd::physics
