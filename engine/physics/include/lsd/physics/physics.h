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

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Returns the body's index (0, 1, 2... in the order added).
    u32 add_body(const BodyDesc& desc);
    // Position + rotation of a body's centre after the last step.
    Mat4 body_transform(u32 body) const;
    u32 body_count() const;

    void set_gravity(f32 metres_per_second2);
    void step(f32 dt);

    // The player. Velocity is horizontal (x, y); jump starts a jump if on the ground.
    void create_character(const CharacterDesc& desc);
    void move_character(Vec3 velocity, bool jump, f32 jump_speed, f32 dt);
    void teleport_character(Vec3 feet);
    Vec3 character_feet() const;
    bool character_on_ground() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace lsd::physics
