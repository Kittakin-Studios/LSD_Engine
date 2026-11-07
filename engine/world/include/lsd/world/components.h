#pragma once
// Built-in components. Each is a reflected plain struct, so the Details panel, level files, undo
// and Play-in-Editor copies all work from the same type table with no per-component code.
#include "lsd/foundation/math.h"
#include "lsd/reflect/color.h"

namespace lsd::world {

// Every entity has one (like Unreal's root scene component). Relative to the parent, if any.
LSD_STRUCT(Component, Category = "Common", Tooltip = "Where the object is, how it is turned and its size")
struct Transform {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Where it is, relative to its parent (or the world)") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "X, then Y, then Z (same as Blender's XYZ Euler)") Vec3 rotation;
    LSD_PROPERTY(Edit, Tooltip = "Size along each axis (1 = as made)") Vec3 scale = {1, 1, 1};
};

LSD_STRUCT(Component, Category = "Rendering", Tooltip = "Shows a mesh (a model or a basic shape), with material, collision and shadows")
struct StaticMesh {
    LSD_GENERATED()
    // Edited with the editor's mesh picker (asset combo + drag-drop), so saved but not shown as text.
    LSD_PROPERTY(Save, Tooltip = "A mesh asset's GUID, or a basic shape: Cube, Sphere, Plane") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Drag a material from the Content Browser onto the object in the viewport, or pick it here") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Tints the material (white = as the material is)") LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit, Tooltip = "Blocks the player in Play") bool collision = true;
    LSD_PROPERTY(Edit, Tooltip = "A jump-through platform: the player passes up and sideways through it and lands on its top (Collision on)") bool one_way = false;
    LSD_PROPERTY(Edit, Widget = "Choice:Box,Mesh", Tooltip = "Box: the bounds (fast). Mesh: the triangles themselves (arches, ramps, stairs; Unreal's Use Complex As Simple)")
    u32 collision_shape = 0;
    LSD_PROPERTY(Edit, Widget = "Asset:PhysicalMaterial", Tooltip = "The surface's friction and bounciness") char physical_material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Box-projected UVs from the object's size: one texture tile per metre on every face, however it is scaled (blockout walls and floors)")
    bool box_uv = false;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Not drawn (nor shadowed) farther than this from the camera; 0 = always (Unreal's Max Draw Distance)")
    f32 max_draw_distance = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Build Lighting bakes a lightmap for it (Unreal's Mobility: Static): baked shadows and bounced light. Off for things that move; physics bodies and movers never get one")
    bool static_lighting = true;
};

// Volumes are boxes the size of the entity's scale (a 1 m cube from z = 0 to 1, like the Cube shape),
// drawn only in the editor.

// Invisible wall: blocks the player (Unreal's Blocking Volume).
LSD_STRUCT(Component, Category = "Volumes", Tooltip = "An invisible wall the player and physics cannot pass")
struct BlockingVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: the player and physics pass through") bool enabled = true;
};

// Sends the player back to the start when entered (Unreal's Kill Z Volume).
LSD_STRUCT(Component, Category = "Volumes", Tooltip = "The player respawns on entering it (pits, lava, out of bounds)")
struct KillVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: entering it does nothing") bool enabled = true;
};

// Tells scripts when the player or a simulated object enters or leaves it (Unreal's Trigger Box):
// on_overlap_begin(self, other) / on_overlap_end(self, other) run on both objects' scripts.
LSD_STRUCT(Component, Category = "Volumes", Tooltip = "Tells scripts when something enters or leaves its box (on_overlap_begin / end)")
struct TriggerVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: no overlap events") bool enabled = true;
    LSD_PROPERTY(Edit, Widget = "Asset:Sound", Tooltip = "Played here when something enters (no script needed)") char enter_sound[33] = "";
};

// Changes how the world sounds while the listener is inside (Unreal's Audio Volume): its own
// reverb, and sounds on the other side of its walls muffled (inside ones heard from outside too).
LSD_STRUCT(Component, Category = "Volumes", Tooltip = "Reverb for sounds heard inside its box")
struct AudioVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: its reverb does not apply") bool enabled = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Reverb on everything heard while inside") f32 reverb = 0.4f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Reverb size: 0 = a small room, 1 = a cathedral") f32 room_size = 0.7f;
    LSD_PROPERTY(Edit, Units = "Hz", Min = 100, Max = 20000, Tooltip = "Sounds through its walls are muffled to this; 20000 = not muffled") f32 muffle = 800.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "How long the reverb takes to change when entering or leaving") f32 fade = 0.5f;
    LSD_PROPERTY(Edit, Tooltip = "Where volumes overlap, the highest priority wins (a small room inside a big cave)") i32 priority = 0;
};

// A sound in the level (Unreal's Ambient Sound): ambience, a river, a crackling torch. Drag a
// sound from the Content Browser into the viewport to make one.
LSD_STRUCT(Component, Category = "Audio", Tooltip = "Plays a sound from this spot, louder when near")
struct SoundEmitter {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Sound", Tooltip = "The sound it plays (a Sound or Sound Cue)") char sound[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Starts when Play starts; it follows the object if it moves") bool auto_play = true;
};

// A particle effect in the level (Unreal's Niagara Component): plays a Particle System data asset
// at the object. Drag a Particle System from the Content Browser into the viewport to make one.
LSD_STRUCT(Component, Category = "Effects", Tooltip = "Plays a Particle System here")
struct ParticleEffect {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:ParticleSystem", Tooltip = "The Particle System it plays") char system[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Emitting; turn off (Details, Lua lsd.set) to let the particles die out") bool active = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Slow motion / fast forward") f32 time_scale = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Plays in the editor too (preview while placing)") bool preview = true;
    LSD_PROPERTY(Edit, Tooltip = "In Play, the object is deleted once the effect has finished (every emitter non-looping and its particles gone): explosions, impacts. lsd.spawn sets it")
    bool auto_destroy = false;
};

// Where Play puts the player; facing = the entity's Z rotation.
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Where the default player appears when there is no Character")
struct PlayerStart {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Height of the player's eyes above the floor (without a Character)") f32 eye_height = 1.7f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Walking speed of the default player (without a Character)") f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Upward speed of a jump (higher jumps higher)") f32 jump_speed = 5.0f;
};

// Simulated by physics in Play: falls, collides, can be pushed. The shape is fitted to the mesh's bounds.
LSD_STRUCT(Component, Category = "Physics", Tooltip = "Physics: it falls, collides, can be pushed (or moves by script when Kinematic)")
struct RigidBody {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Box,Sphere,Capsule,Convex", Tooltip = "Collision shape, fitted to the mesh bounds; Convex wraps the mesh's points") u32 shape = 0;
    LSD_PROPERTY(Edit, Tooltip = "2D physics: moves only in its X-Z plane and turns only around Y (Unreal's Constrain to Plane)") bool lock_2d = false;
    LSD_PROPERTY(Edit, Units = "kg", Min = 0.01, Tooltip = "Weight: heavier objects push lighter ones and are harder to move") f32 mass = 10.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Grip on surfaces: 0 = ice, 1 = rubber") f32 friction = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How much it bounces: 0 = none, 1 = a superball") f32 bounciness = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:PhysicalMaterial", Tooltip = "Friction and bounciness from a Physical Material (overrides the two above)") char physical_material[33] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Block All,Debris", Tooltip = "Debris hits only the static level: not the player, not other moving objects")
    u32 collision_preset = 0;
    LSD_PROPERTY(Edit, Tooltip = "Moved by its Transform (scripts, timelines, animation), pushing what it meets; not pushed itself")
    bool kinematic = false;
};

// Surface properties shared by many objects (Unreal's Physical Material): ice, rubber, mud.
LSD_STRUCT(Category = "Physics")
struct PhysicalMaterial {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "Grip on surfaces: 0 = ice, 1 = rubber, 2 = sticky") f32 friction = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How much it bounces: 0 = none, 1 = a superball") f32 bounciness = 0.0f;
};

// Holds this RigidBody to another object or to the world (Unreal's Physics Constraint): doors,
// chains, pendulums, drawers, fans. Pivot and axis are in this object's local space (mesh units,
// so (0, -0.5, 0) is the edge of a cube); the yellow marker in the viewport shows them.
LSD_STRUCT(Component, Category = "Physics", Tooltip = "Links two bodies: a hinge, a slider, a ball joint, a spring, a fixed weld")
struct PhysicsJoint {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Fixed,Hinge,Ball,Slider,Cone",
                 Tooltip = "Fixed: welded. Hinge: turns about the axis. Ball: turns any way. Slider: moves along the axis. Cone: swings within Cone Angle of the axis, twist limited (ragdolls, lamps)")
    u32 type = 1;
    LSD_PROPERTY(Edit, Tooltip = "Name of the object to hold on to; empty = the world") char other[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "Where it turns, local") Vec3 pivot;
    LSD_PROPERTY(Edit, Tooltip = "Hinge or slider axis, local") Vec3 axis = {0, 0, 1};
    LSD_PROPERTY(Edit, Tooltip = "The joint only turns / slides between Limit Min and Limit Max") bool limited = false;
    LSD_PROPERTY(Edit, Tooltip = "Degrees (hinge) or metres (slider) from where it starts") f32 limit_min = -45.0f;
    LSD_PROPERTY(Edit, Tooltip = "Degrees (hinge) or metres (slider) from where it starts") f32 limit_max = 45.0f;
    LSD_PROPERTY(Edit, Tooltip = "Hinge deg/s or slider m/s; 0 = no motor") f32 motor_speed = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "N m (hinge) or N (slider)") f32 motor_strength = 1000.0f;
    LSD_PROPERTY(Edit, Units = "Hz", Min = 0, Tooltip = "Limits give like a spring this many times a second (hinge, slider); 0 = hard stops") f32 spring_frequency = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 bounces forever, 1 settles without overshoot") f32 spring_damping = 0.5f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 179, Tooltip = "Cone: how far it swings from the axis; the limits bound its twist") f32 cone_angle = 45.0f;
    LSD_PROPERTY(Edit, Units = "N", Min = 0, Tooltip = "Snaps when pulled harder than this (scripts get on_joint_break); 0 = never") f32 break_force = 0.0f;
};

// The player's character (Unreal's Character + Character Movement + its input setup). In Play the
// first Character in the level is possessed; its input comes from the mapping context and actions
// picked here, so no code is needed to wire it.
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "A player the keyboard, mouse or gamepad controls: walks, jumps, crouches, swims")
struct Character {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:InputMappingContext", Tooltip = "Which keys drive this character (an Input Mapping Context); empty = WASD, mouse, Space") char mapping_context[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "Axis 2D: x = right, y = forward") char move_action[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "Axis 2D: x = turn right, y = look up") char look_action[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "The Input Action that jumps") char jump_action[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "2D side-scroller: Move's x walks along X only (no turning), Jump jumps; the object's Sprite faces the way it walks") bool side_scroller = false;
    LSD_PROPERTY(Edit, Category = "Animation",
                 Tooltip = "Built-in locomotion by Motion Matching (like Unreal's Game Animation Sample): the Skeletal Mesh picks the frame of its locomotion clips that best fits where the input takes the character. Off: the Skeletal Mesh's own Anim Graph (or Clip)")
    bool motion_matching = false;
    LSD_PROPERTY(Edit, Category = "Animation", Widget = "Asset:MotionDatabase",
                 Tooltip = "The clips it picks from (a Motion Database); empty: the locomotion clips next to the character's mesh (Idle, Walk, Run, Jog, Strafe, Turn, Start, Stop... in the name)")
    char motion_database[33] = "";
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Walking speed") f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Upward speed of a jump (higher jumps higher)") f32 jump_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89, Tooltip = "Steepest ground it can walk up") f32 max_slope = 45.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Tallest step it climbs without jumping (stairs, kerbs)") f32 step_height = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.2, Tooltip = "Height of its capsule (what collides)") f32 height = 1.8f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Tooltip = "Radius of its capsule (how close it gets to walls)") f32 radius = 0.35f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Height of the view above the feet (first person, no Camera on the Character)") f32 eye_height = 1.65f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Tooltip = "Turn per unit of Look (per pixel for the mouse)") f32 look_sensitivity = 0.15f;
    LSD_PROPERTY(Edit, Tooltip = "Moving the mouse up looks down") bool invert_look_y = false;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Spring arm length behind the head: 0 = first person") f32 camera_distance = 0.0f;
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Moves the view from the head (or from a Camera on this object): x forward, y left, z up. 0, -0.5, 0.2 looks over the right shoulder (Unreal's Socket Offset). Or give this object a child with a Camera and move that in the viewport")
    Vec3 camera_offset{0.0f, 0.0f, 0.0f};
    LSD_PROPERTY(Edit, Category = "Crouch", Widget = "Asset:InputAction", Tooltip = "Held: crouch (stands up again when there is room)") char crouch_action[33] = "";
    LSD_PROPERTY(Edit, Category = "Crouch", Units = "m", Min = 0.3, Tooltip = "Capsule height while crouched") f32 crouch_height = 1.1f;
    LSD_PROPERTY(Edit, Category = "Crouch", Units = "m/s", Min = 0, Tooltip = "Walking speed while crouched") f32 crouch_speed = 2.5f;
    LSD_PROPERTY(Edit, Category = "Swim", Units = "m/s", Min = 0, Tooltip = "Inside a Water Volume: moves where you look, Jump rises, Crouch dives") f32 swim_speed = 3.0f;
};

// Water the Character swims in (Unreal's Physics Volume with Water Volume on): the unit cube scaled
// by the transform, like the other volumes. Swimming starts when the chest is under.
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Water the Character swims in")
struct WaterVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: no swimming here") bool enabled = true;
};

// A camera the game can look through (Unreal's Camera Component / Camera Actor): along its +X.
// Auto Activate: Play starts looking through it instead of the player's head; lsd.set_view(e)
// switches to any camera (0: back to the player), Timelines use them for camera cuts.
LSD_STRUCT(Component, Category = "Camera", Tooltip = "A view to look through: cutscenes, fixed views, or the player's view on a Character (first / third person)")
struct Camera {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "deg", Min = 10, Max = 170, Tooltip = "Vertical field of view") f32 fov = 60.0f;
    LSD_PROPERTY(Edit, Tooltip = "Play starts looking through this camera. Off on a Camera that is (or is a child of) the player's Character: the player's view starts where you put it, and mouse look turns it") bool auto_activate = false;
    LSD_PROPERTY(Edit, Tooltip = "No perspective: sizes stay the same at any distance (2D games, strategy views)") bool orthographic = false;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.1, Max = 1000, Tooltip = "Orthographic: how many metres of the world fit the screen's height") f32 ortho_height = 10.0f;
    // On the player's Character (or a child of it, not Auto Activate): how the player sees.
    LSD_PROPERTY(Edit, Category = "Player View", Widget = "Choice:First Person,Third Person",
                 Tooltip = "On the player's Character: First Person looks from the Camera (or Follow Bone); Third Person looks from behind it on a spring arm of Arm Length. Scripts switch it: lsd.set_camera_view(\"first\" | \"third\")")
    u32 view = 0;
    LSD_PROPERTY(Edit, Category = "Player View", Units = "m", Min = 0, Max = 50, Tooltip = "Third Person: how far behind the Camera the view sits (pulled in when a wall is in the way)") f32 arm_length = 3.0f;
    LSD_PROPERTY(Edit, Category = "Player View", Tooltip = "First Person: the view follows this bone (or socket) of the Character's Skeletal Mesh so head bob and crouching move it; UE4 / UE5 rigs: head. Empty: where the Camera is")
    char follow_bone[32] = "";
    LSD_PROPERTY(Edit, Category = "Player View", Widget = "Choice:Mouse Look,Orbit Bone,Ride Bone",
                 Tooltip = "Mouse Look: the mouse turns the view; the Camera's own rotation tilts it. Orbit Bone: the mouse circles the view around Follow Bone at Arm Length. Ride Bone: the Camera keeps where you put it relative to Follow Bone and moves only with the bone's animation (the mouse does not turn it)")
    u32 motion = 0;
};

// Another level placed in this one (Unreal's Level Instance and level streaming): its objects appear
// under this object (they move with it), are not saved into this level, and are picked as this
// object; open that level to edit them. In Play it can stream: loaded when the player comes within
// Load Distance, unloaded a little beyond it. Scripts: lsd.load_sublevel / unload_sublevel.
LSD_STRUCT(Component, Category = "Level", Tooltip = "Places another level here; it can stream in and out by distance")
struct SubLevel {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Level", Tooltip = "The level to place here") char level[33] = "";
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 100000, Tooltip = "Play: loaded only while the player is this near (0 = always loaded)") f32 load_distance = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Play: loaded when Play starts (off: only by distance or a script)") bool load_at_start = true;
    LSD_PROPERTY(Edit, Tooltip = "Show it in the editor") bool show_in_editor = true;
};

// Moves the object by itself (Unreal's Interp To Movement and spline routes): along a direction, back
// and forth to an offset, or along a Spline object's path, optionally turning. It can wait for its
// start (Begin Play, the player entering a Trigger Volume, a time of the game, or a script) and run
// again every so many minutes (120 = every 2 hours). It is solid: what stands on it rides along.
LSD_STRUCT(Component, Category = "Movement", Tooltip = "Moves by itself: a direction, back and forth, or along a spline; on start, a trigger or a timer")
struct Mover {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Direction,Back and Forth,Spline", Tooltip = "Direction: forever (or Duration) at Velocity. Back and Forth: to Offset and back. Spline: along a Spline object's route") u32 path = 1;
    LSD_PROPERTY(Edit, Units = "m/s", Tooltip = "Direction: the velocity, any vector (x, y, z)") Vec3 velocity{2.0f, 0.0f, 0.0f};
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Back and Forth: where it goes, from where it starts") Vec3 offset{0.0f, 0.0f, 4.0f};
    LSD_PROPERTY(Edit, Tooltip = "Spline: the name of the object with the Spline (its route)") char spline[64] = "";
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Back and Forth / Spline") f32 speed = 3.0f;
    LSD_PROPERTY(Edit, Widget = "Choice:Once,Loop,Ping-Pong", Tooltip = "Once: stops at the end. Loop: starts over. Ping-Pong: comes back") u32 repeat = 2;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Direction: stops after this long (0 = never)") f32 duration = 0.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Waits this long at each end (stations, lift floors)") f32 pause = 0.0f;
    LSD_PROPERTY(Edit, Units = "deg/s", Tooltip = "Turns while it moves (a rotating platform, a windmill)") Vec3 spin{0.0f, 0.0f, 0.0f};
    LSD_PROPERTY(Edit, Tooltip = "Spline: turns to face along its route (trains, cars)") bool face_forward = true;
    LSD_PROPERTY(Edit, Category = "Start", Widget = "Choice:Begin Play,Trigger,Game Time,Script", Tooltip = "When it starts: at once, when the player enters Trigger, at Start Time, or when a script calls lsd.start_mover") u32 start = 0;
    LSD_PROPERTY(Edit, Category = "Start", Tooltip = "Trigger: the Trigger Volume's name") char trigger[64] = "";
    LSD_PROPERTY(Edit, Category = "Start", Units = "s", Min = 0, Tooltip = "Game Time: seconds after Play starts") f32 start_time = 10.0f;
    LSD_PROPERTY(Edit, Category = "Start", Units = "min", Min = 0, Tooltip = "Runs its route again from the start every so many minutes (60 = hourly); 0 = no") f32 repeat_minutes = 0.0f;
};

// A vehicle the player drives (Unreal's Chaos Vehicle, arcade-style): E near it gets in or out; W / S
// throttle and reverse, A / D steer, Shift boost, Ctrl or Space handbrake (a flying one: Space up,
// Ctrl down). It hugs the ground, stops at walls, and the camera follows behind it.
// One wheel of a physics Vehicle (Unreal's Wheel Setup): where it sits and what it does; Object names
// a child to spin, steer and bounce with it (a wheel mesh, like Unreal's wheel bones).
LSD_STRUCT()
struct VehicleWheel {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "A child object turned with the wheel (its pivot at the wheel's centre); empty: none") char object[64] = "";
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Where its suspension is fixed, from the vehicle's origin (x forward, y left, z up)") Vec3 position{1.4f, 0.8f, 0.3f};
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 5, Tooltip = "0 = the vehicle's Wheel Radius") f32 radius = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Turns with the steering (front wheels)") bool steers = false;
    LSD_PROPERTY(Edit, Tooltip = "The engine turns it") bool driven = true;
    LSD_PROPERTY(Edit, Tooltip = "Locked by the handbrake (Space; usually the rear wheels)") bool handbrake = false;
};

LSD_STRUCT(Component, Category = "Movement", Tooltip = "A vehicle the player enters and drives (WASD, Shift boost, Ctrl brake)")
struct Vehicle {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Ground,Flying", Tooltip = "Ground: a car, cart or tank on the floor. Flying: a ship or drone that climbs") u32 kind = 0;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Top speed forward") f32 max_speed = 20.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Top speed backward") f32 reverse_speed = 6.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0, Tooltip = "How quickly it speeds up") f32 acceleration = 8.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0, Tooltip = "How quickly it slows down when braking") f32 braking = 20.0f;
    LSD_PROPERTY(Edit, Units = "deg/s", Min = 0, Tooltip = "How fast it steers at full lock") f32 turn_rate = 90.0f;
    LSD_PROPERTY(Edit, Min = 1, Max = 5, Tooltip = "Shift multiplies the top speed by this") f32 boost = 1.6f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Flying: how fast Space / Ctrl climb and sink") f32 climb_speed = 6.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.5, Tooltip = "How near the player must be to get in with E") f32 enter_distance = 3.0f;
    LSD_PROPERTY(Edit, Tooltip = "Where the driver sits, in the vehicle's own space (0,0,1 = on top of a unit cube)") Vec3 seat{0.0f, 0.0f, 1.0f};
    LSD_PROPERTY(Edit, Tooltip = "Passenger seats, in the vehicle's own space (Unreal's seats): in multiplayer, players getting in once the driver's seat is taken sit here")
    List<Vec3, 8> passenger_seats;
    LSD_PROPERTY(Edit, Units = "m", Min = 1, Max = 50, Tooltip = "How far behind the vehicle the driver's view is") f32 camera_distance = 8.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 30, Tooltip = "How high above the vehicle the driver's view is") f32 camera_height = 3.0f;
    // Physics model (ground vehicles): a dynamic body on wheels (Unreal's Chaos Wheeled Vehicle Movement).
    LSD_PROPERTY(Edit, Category = "Physics", Widget = "Choice:Arcade,Physics",
                 Tooltip = "Arcade: moved directly (simple, never flips). Physics: a body on wheels with suspension, an engine and gears; it rolls, bumps and slides (ground vehicles)")
    u32 model = 0;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "kg", Min = 50, Max = 100000, Tooltip = "How heavy it is (a car about 1500)") f32 mass = 1500.0f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "N m", Min = 10, Max = 100000, Tooltip = "The engine's pull at full throttle (more: faster off the line, up hills)") f32 engine_torque = 500.0f;
    LSD_PROPERTY(Edit, Category = "Physics", Min = 1000, Max = 20000, Tooltip = "Engine speed limit; the gearbox shifts up near it") f32 max_rpm = 6000.0f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "N m", Min = 0, Max = 100000, Tooltip = "Each wheel's brake (Ctrl)") f32 brake_torque = 1500.0f;
    LSD_PROPERTY(Edit, Category = "Physics", Widget = "Choice:Rear,Front,All Wheel", Tooltip = "Which wheels the engine drives (the automatic wheels; listed wheels say for themselves)") u32 drive = 0;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "deg", Min = 0, Max = 80, Tooltip = "How far the front wheels turn at full lock") f32 max_steer_angle = 30.0f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "m", Min = 0.05, Max = 5, Tooltip = "Wheel radius (the automatic wheels, and listed ones with Radius 0)") f32 wheel_radius = 0.35f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "m", Min = 0.02, Max = 3, Tooltip = "Tyre width") f32 wheel_width = 0.25f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "m", Min = 0.01, Max = 3, Tooltip = "Suspension travel, fully up to fully down") f32 suspension_length = 0.3f;
    LSD_PROPERTY(Edit, Category = "Physics", Units = "Hz", Min = 0.1, Max = 20, Tooltip = "Suspension stiffness: higher is firmer (a race car 2.5, a truck 1)") f32 suspension_stiffness = 1.5f;
    LSD_PROPERTY(Edit, Category = "Physics", Min = 0, Max = 1, Tooltip = "0 bounces on and on, 1 settles at once") f32 suspension_damping = 0.5f;
    LSD_PROPERTY(Edit, Category = "Physics", Tooltip = "The wheels (Unreal's Wheel Setups); empty: four at the corners of its box")
    List<VehicleWheel, 8> wheels;
};

// A ride the player hitches without driving (trains, ferries, lifts, cable cars): E near it gets on or
// off, and the player travels on its seat. It moves with a Mover or Spline Follower on the same object.
LSD_STRUCT(Component, Category = "Movement", Tooltip = "Something the player rides along its route without driving (trains, lifts, boats)")
struct Transport {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0.5, Tooltip = "How near the player must be to get on") f32 enter_distance = 4.0f;
    LSD_PROPERTY(Edit, Tooltip = "Where the rider stands, in the object's own space (0,0,1 = on top of a unit cube)") Vec3 seat{0.0f, 0.0f, 1.0f};
    LSD_PROPERTY(Edit, Tooltip = "The rider can look around while riding (off: faces the way it travels)") bool free_look = true;
};

// This object replicates in multiplayer (Unreal's Replicates + Replicate Movement): the server's copy
// is the real one and every client shows it, smoothly. Ignored when multiplayer is off.
LSD_STRUCT(Component, Category = "Multiplayer", Tooltip = "Multiplayer: the server sends this object's movement to every player")
struct NetReplicated {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Send its position and rotation to the clients (off: it is created and destroyed everywhere, but not moved)") bool replicate_movement = true;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Players farther than this from it get no updates of its movement (Unreal's Net Cull Distance: saves bandwidth in big levels); 0 = always sent")
    f32 relevancy_distance = 0.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0,
                 Tooltip = "A player's own object (an avatar, a driven car): moves faster than this are refused by the server and it is put back (stops speed and teleport hacks); 0 = no check")
    f32 max_speed = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "More values the server sends when they change (Unreal's Replicated properties): Component.field, comma-separated, e.g. StaticMesh.color, PointLight.intensity, Tags.tags")
    char replicated_fields[256] = "";
    LSD_PROPERTY(Visible, Tooltip = "Who moves it: 0 = the server, else that player's machine (their avatar)") u32 owner = 0;
    LSD_PROPERTY(Visible, Tooltip = "Its id on the network (from its name for objects placed in the level)") u32 net_id = 0;
    LSD_PROPERTY(Visible, Tooltip = "What the server spawned it from, so clients make the same") char spawned_from[64] = "";
};

// A 2D image in the world (Unreal's Paper2D Sprite and Flipbook): a texture, or one cell of a sprite
// sheet, on a 1 m quad in the X-Z plane facing -Y (a side-on game looks along +Y: the Front view),
// pivot at the bottom centre; the Transform's scale sizes it. With Frame Count > 1 it plays that
// many cells from Frame at FPS (a flipbook); scripts switch animations by setting Frame.
// A tileset tile's collision in a Tile Map (Paper2D's per-tile collision).
LSD_STRUCT(Category = "2D")
struct TileShape {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Full,None,Top Half,Bottom Half,Slope Up,Slope Down,One Way",
                 Tooltip = "Full: the whole square blocks. None: drawn only (decoration). Halves: the upper or lower half. Slopes: a ramp rising to the right (Up) or to the left (Down). One Way: the player jumps up through it and lands on top")
    u32 shape = 0;
};

// One frame of a Flipbook: a cell of the sprite's sheet, how long it shows, and an event.
LSD_STRUCT(Category = "2D")
struct FlipbookFrame {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The sheet cell shown (left to right, then down)") u32 cell = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0.001, Tooltip = "How long it shows") f32 duration = 0.1f;
    LSD_PROPERTY(Edit, Tooltip = "In Play, the sprite's scripts run this function when the frame starts (footstep, swing...); empty = none") char event[32] = "";
};

// A Flipbook asset (Unreal's Paper2D Flipbook): frames of a sprite sheet, each with its own time and an optional event.
LSD_STRUCT(Category = "2D")
struct Flipbook {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The frames in order") List<FlipbookFrame, 256> frames;
    LSD_PROPERTY(Edit, Tooltip = "Starts over when it ends; off: stops on the last frame") bool loop = true;
    // The Flipbook window's preview only (sprites playing it use their own sheet).
    LSD_PROPERTY(Edit, Category = "Preview", Widget = "Asset:Texture", Tooltip = "The sprite sheet the Flipbook window shows the cells from") char preview_texture[33] = "";
    LSD_PROPERTY(Edit, Category = "Preview", Min = 1, Max = 64, Tooltip = "Cells across the preview sheet") u32 preview_columns = 4;
    LSD_PROPERTY(Edit, Category = "Preview", Min = 1, Max = 64, Tooltip = "Cells down the preview sheet") u32 preview_rows = 1;
};

// One more layer of a Tile Map (Unreal's Paper2D Tile Map layers): decoration in front of or behind
// the main tiles, painted from the same tileset.
LSD_STRUCT(Category = "2D")
struct TileLayer {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Its name in the 2D mode's layer list (Background, Foreground...)") char name[32] = "Layer";
    LSD_PROPERTY(Edit, Tooltip = "Drawn; off: hidden (still kept)") bool visible = true;
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Along +Y from the main tiles: positive behind them (backgrounds), negative in front (foreground grass)") f32 depth = 0.1f;
    LSD_PROPERTY(Edit, Tooltip = "Its tiles block in Play too (with the map's Collision on)") bool collision = false;
    LSD_PROPERTY(Save) List<i32, 4096> tiles;  // as the map's own tiles
};

LSD_STRUCT(Component, Category = "2D", Tooltip = "A flat image for 2D games, with sprite-sheet frames and flipbook animation")
struct Sprite {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "The image (or sprite sheet) it shows") char texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour multiplied over the image (white = as is)") LinearColor tint = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Category = "Sheet", Min = 1, Max = 64, Tooltip = "Cells across the sprite sheet") u32 columns = 1;
    LSD_PROPERTY(Edit, Category = "Sheet", Min = 1, Max = 64, Tooltip = "Cells down") u32 rows = 1;
    LSD_PROPERTY(Edit, Category = "Sheet", Tooltip = "The cell shown, or the animation's first (left to right, then down)") u32 frame = 0;
    LSD_PROPERTY(Edit, Category = "Flipbook", Min = 1, Max = 4096, Tooltip = "Cells the animation plays from Frame; 1 = a still sprite") u32 frame_count = 1;
    LSD_PROPERTY(Edit, Category = "Flipbook", Min = 0, Max = 120, Tooltip = "Frames a second") f32 fps = 12.0f;
    LSD_PROPERTY(Edit, Category = "Flipbook", Tooltip = "The flipbook starts over when it ends") bool loop = true;
    LSD_PROPERTY(Edit, Category = "Flipbook", Widget = "Asset:Flipbook", Tooltip = "A Flipbook asset (frames of this sheet with their own durations and events) played instead of Frame / Frame Count / FPS")
    char flipbook[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Mirror left-right (a side-scroller Character sets it from the way it walks)") bool flip_x = false;
    LSD_PROPERTY(Edit, Tooltip = "Mirror the image upside down") bool flip_y = false;
    LSD_PROPERTY(Edit, Tooltip = "Whole texels, no blur (pixel art)") bool pixel_art = true;
    LSD_PROPERTY(Edit, Tooltip = "Lit by the level's lights; off: its own colours, like most 2D games") bool lit = false;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Pixels with less alpha are cut out") f32 alpha_cutoff = 0.5f;
    LSD_PROPERTY(Edit, Tooltip = "Soft edges and see-through colours (glass, smoke, glows): blended over what is behind, drawn far to near after the solid ones. Off: cut out at Alpha Cutoff")
    bool translucent = false;
    LSD_PROPERTY(Edit, Tooltip = "Translucent sprites at the same depth: higher draws on top (Unreal's Translucency Sort Priority)") i32 sort_order = 0;
    LSD_PROPERTY(Edit, Tooltip = "Lit sprites cast shadows from the level's lights (its cut-out shape)") bool cast_shadows = true;
    LSD_PROPERTY(Edit, Tooltip = "Blocks in Play: a box the sprite's size, 1 m deep (a RigidBody uses the same box)") bool collision = false;
    LSD_PROPERTY(Edit, Tooltip = "With Collision: a jump-through platform (the player lands on it from above)") bool one_way = false;
    // On a side-scroller Character (Paper2D Character's flipbooks): Frame / Frame Count are the idle animation.
    LSD_PROPERTY(Edit, Category = "Character Animation", Tooltip = "On a side-scroller Character: the first cell of the walk animation") u32 walk_frame = 0;
    LSD_PROPERTY(Edit, Category = "Character Animation", Min = 0, Max = 4096, Tooltip = "Cells of the walk animation; 0 = keep the idle one while walking") u32 walk_count = 0;
    LSD_PROPERTY(Edit, Category = "Character Animation", Tooltip = "On a side-scroller Character: the first cell shown in the air (jumping, falling)") u32 jump_frame = 0;
    LSD_PROPERTY(Edit, Category = "Character Animation", Min = 0, Max = 4096, Tooltip = "Cells of the in-air animation; 0 = keep the idle or walk one") u32 jump_count = 0;
    LSD_PROPERTY(Edit, Category = "Character Animation", Widget = "Asset:Flipbook", Tooltip = "On a side-scroller Character: a Flipbook played while walking (instead of Walk Frame / Count)")
    char walk_flipbook[33] = "";
    LSD_PROPERTY(Edit, Category = "Character Animation", Widget = "Asset:Flipbook", Tooltip = "On a side-scroller Character: a Flipbook played in the air (instead of Jump Frame / Count)")
    char jump_flipbook[33] = "";
};

// A grid of tiles from a tileset image (Unreal's Paper2D Tile Map), painted in the 2D level mode:
// drawn as one mesh in the X-Z plane from its bottom-left corner; solid tiles block in Play.
LSD_STRUCT(Component, Category = "2D", Tooltip = "A grid of tiles from a tileset, painted in the 2D mode")
struct TileMap {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "The tileset: an image of equal tiles") char tileset[33] = "";
    LSD_PROPERTY(Edit, Min = 1, Max = 256, Tooltip = "Tiles across the tileset image") u32 tileset_columns = 8;
    LSD_PROPERTY(Edit, Min = 1, Max = 256, Tooltip = "Tiles down the tileset image") u32 tileset_rows = 8;
    LSD_PROPERTY(Edit, Min = 1, Max = 256, Tooltip = "Tiles across the map (width x height at most 4096)") u32 width = 32;
    LSD_PROPERTY(Edit, Min = 1, Max = 128, Tooltip = "Rows of tiles") u32 height = 16;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Max = 100, Tooltip = "Size of one tile in the world") f32 tile_size = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Colour multiplied over every tile (white = as is)") LinearColor tint = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Tooltip = "Whole texels, no blur (pixel art)") bool pixel_art = true;
    LSD_PROPERTY(Edit, Tooltip = "Lit by the level's lights; off: its own colours") bool lit = false;
    LSD_PROPERTY(Edit, Tooltip = "Painted tiles block in Play (rows of tiles become boxes)") bool collision = true;
    LSD_PROPERTY(Edit, Tooltip = "Lit maps cast shadows from the level's lights") bool cast_shadows = true;
    LSD_PROPERTY(Edit, Category = "Collision", Tooltip = "Each tileset tile's collision, by tile number (the 2D mode's palette sets it for the chosen tile); missing ones are Full")
    List<TileShape, 256> tile_shapes;
    // Row by row from the bottom-left: the tileset cell + 1 (cells count left to right, then down), 0 = empty.
    LSD_PROPERTY(Save) List<i32, 4096> tiles;
    LSD_PROPERTY(Edit, Tooltip = "More layers over or under the main tiles (the 2D mode's Layer list paints them)") List<TileLayer, 3> layers;
};

// A picture projected onto whatever is inside its box (Unreal's Decal Actor): blood, dirt, cracks,
// signs, road markings. The box is the unit cube scaled by the transform (like the volumes); it
// projects down its local -Z onto surfaces facing up its +Z. The texture's alpha cuts it out.
LSD_STRUCT(Component, Category = "Rendering", Tooltip = "Projects an image onto surfaces inside its box (stains, signs, cracks)")
struct Decal {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "The image projected onto surfaces inside its box") char texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Multiplies the texture; alpha = opacity") LinearColor tint = LinearColor::hex(0xFFFFFF);
};

// One copy in an Instanced Static Mesh, relative to the object.
LSD_STRUCT()
struct MeshInstance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Where this copy is, relative to the object") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "How this copy is turned") Vec3 rotation;
    LSD_PROPERTY(Edit, Tooltip = "This copy's size") Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Many copies of one mesh drawn in a single call (Unreal's Instanced Static Mesh): rocks, grass,
// fences, crowds of props. Details > Scatter fills it at random over a circle.
// shortcut: no collision and no per-instance picking; built-in Materials only (a Material Graph
// draws each copy on its own).
LSD_STRUCT(Component, Category = "Rendering", Tooltip = "Many copies of one mesh drawn together (foliage, crowds of props)")
struct InstancedStaticMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The mesh every copy shows") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Material for every copy (empty: the mesh's own colour)") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour of every copy (without a material)") LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit, Tooltip = "The copies cast shadows") bool cast_shadows = true;
    LSD_PROPERTY(Edit, Tooltip = "Each copy blocks in Play like a Static Mesh with collision (trees, rocks): a box around the mesh, sized and turned as the copy; off for grass") bool collision = false;
    LSD_PROPERTY(Edit, Tooltip = "The copies (foliage paints them); drawn together in one go") List<MeshInstance, 1024> instances;
};

// An Editable Mesh's point and face (3 or 4 point indices, counter-clockwise seen from outside; d = 0xFFFFFFFF: a triangle).
LSD_STRUCT()
struct EditPoint {
    LSD_GENERATED()
    LSD_PROPERTY(Save) Vec3 p;
};
LSD_STRUCT()
struct EditFace {
    LSD_GENERATED()
    LSD_PROPERTY(Save) u32 a = 0;
    LSD_PROPERTY(Save) u32 b = 0;
    LSD_PROPERTY(Save) u32 c = 0;
    LSD_PROPERTY(Save) u32 d = 0xFFFFFFFFu;
};

// A mesh made in the 3D Modeling mode (Unreal's Modeling Mode, PolyEdit): its points and faces live in
// the level, edited with Extrude, Inset, Bevel, Push / Pull, Delete and Flip, drawn and collided through
// the Static Mesh beside it (mesh "EditMesh:<id>", built from these by the editor and the game).
LSD_STRUCT(Component, Category = "Rendering", Tooltip = "A mesh shaped in the 3D Modeling mode: click a face there, then extrude, inset or bevel it")
struct EditableMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Save) List<EditPoint, 1024> points;
    LSD_PROPERTY(Save) List<EditFace, 1024> faces;
    LSD_PROPERTY(Edit, Min = 0.01, Tooltip = "Texture tiles per metre (its UVs are projected from each face's side)") f32 uv_scale = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Slides the texture along U") f32 uv_offset_u = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Slides the texture along V") f32 uv_offset_v = 0.0f;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "Turns the texture on every face") f32 uv_rotation = 0.0f;
};

// A Landscape's paint layer (Unreal's Landscape Layer Info): a texture tiled over the terrain, tinted.
LSD_STRUCT()
struct LandscapeLayer {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Its name in the Landscape mode's Paint list (Grass, Rock, Sand...)") char name[32] = "Layer";
    LSD_PROPERTY(Edit, Widget = "Asset:Texture", Tooltip = "The texture tiled where this layer is painted (none: the tint alone)") char texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour, multiplied with the texture") LinearColor tint = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "m", Min = 0.1, Tooltip = "Metres per texture tile") f32 tile = 4.0f;
};

// Terrain sculpted in Landscape mode (Unreal's Landscape): a square grid of heights centred on the
// object, drawn and collided through the Static Mesh beside it (its mesh is "Landscape:<id>", built
// from these heights by the editor and the game every time they change).
// shortcut: one 65 x 65 grid saved in the level, one material; tiles (and bigger grids) for big worlds
LSD_STRUCT(Component, Category = "Rendering", Tooltip = "Sculptable terrain (the Landscape mode)")
struct Landscape {
    LSD_GENERATED()
    static constexpr u32 kResolution = 65;  // heights per side
    LSD_PROPERTY(Edit, Units = "m", Min = 1, Tooltip = "Width of the square (the grid stretches with it)") f32 size = 64.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.1, Tooltip = "Metres per texture tile") f32 uv_tile = 4.0f;
    LSD_PROPERTY(Save) List<f32, 4225> heights;  // kResolution x kResolution, row by row (y, then x), metres
    LSD_PROPERTY(Edit, Tooltip = "Paint layers (Landscape mode > Paint). The first covers everything until others are painted over it; 2 or more to paint")
    List<LandscapeLayer, 4> layers;
    LSD_PROPERTY(Save) List<u32, 4225> weights;  // per height: the 4 layers' weights as bytes (layer 0 in the lowest), summing to 255
    LSD_PROPERTY(Save) List<u32, 4225> holes;    // per height: 1 cuts the squares around it (caves, tunnels); empty until the first hole
};

// One more mesh a PCG Volume scatters, picked for a point by its weight against the others.
LSD_STRUCT(Category = "Procedural")
struct PcgMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "Another mesh to scatter (rocks among the trees)") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Its material") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Its colour (without a material)") LinearColor color = LinearColor::hex(0x8A8A8E);
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "How often it is picked against the main mesh's Weight and the others'") f32 weight = 1.0f;
};

// Scatters copies of a mesh on whatever surfaces are inside this box (Unreal's PCG Volume with a
// surface sampler): density, slope and scale rules, the same result for the same seed. The copies go
// to an Instanced Static Mesh on a separate object (`output`; More Meshes to its "Variant N" children),
// rebuilt when a rule or the box changes. An Exclude volume scatters nothing and keeps the others out
// of its box (Unreal's PCG difference / exclusion).
LSD_STRUCT(Component, Category = "Procedural", Tooltip = "Scatters a mesh on the surfaces in its box by density, slope and seed")
struct PcgVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The mesh it scatters") char mesh[40] = "Sphere";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Material for the scattered meshes") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour of the scattered meshes (without a material)") LinearColor color = LinearColor::hex(0x4F8A3C);
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "Copies per 100 square metres of the box's floor (at most 1024)") f32 density = 20.0f;
    LSD_PROPERTY(Edit, Tooltip = "Same seed, same layout; change it for another") u32 seed = 1;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 90, Tooltip = "No copies on surfaces steeper than this") f32 max_slope = 35.0f;
    LSD_PROPERTY(Edit, Min = 0.01, Tooltip = "Smallest random size of a scattered mesh") f32 scale_min = 0.8f;
    LSD_PROPERTY(Edit, Min = 0.01, Tooltip = "Largest random size of a scattered mesh") f32 scale_max = 1.2f;
    LSD_PROPERTY(Edit, Tooltip = "Turn each copy at random around Z") bool random_yaw = true;
    LSD_PROPERTY(Edit, Tooltip = "Tilt copies to the surface (rocks on a slope) instead of standing upright") bool align_to_surface = false;
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "How often the main mesh is picked against More Meshes' weights") f32 weight = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Other meshes scattered with it, each picked by its weight (several kinds of plants, rocks)") List<PcgMesh, 8> more_meshes;
    LSD_PROPERTY(Edit, Tooltip = "An exclusion box: it scatters nothing, and the other PCG Volumes leave its box empty (paths, clearings, buildings)") bool exclude = false;
    LSD_PROPERTY(Visible, Tooltip = "The object holding the copies") char output[64] = "";
};

// Keeps this object on a socket of another object's Skeletal Mesh (Unreal's Attach to Socket): a
// sword in a hand follows the animation. The socket comes from the mesh (Mesh Editor > Sockets).
LSD_STRUCT(Component, Category = "Animation", Tooltip = "Sticks this object to a socket or bone of another object's skeleton (weapons in hands)")
struct AttachToSocket {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The object with the Skeletal Mesh, by name") char target[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "The socket (or bone) on the target's skeleton to stick to") char socket[32] = "";
};

// Plants a Skeletal Mesh's feet on the ground under them while playing (Unreal's foot IK): each
// foot is raised onto steps and slopes with two-bone IK. Bones by name, as in the imported skeleton.
LSD_STRUCT(Component, Category = "Animation", Tooltip = "Plants the skeleton's feet on uneven ground")
struct FootIK {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The left foot bone (UE rigs: foot_l)") char left_foot[48] = "foot_l";
    LSD_PROPERTY(Edit, Tooltip = "The right foot bone (UE rigs: foot_r)") char right_foot[48] = "foot_r";
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 2, Tooltip = "How high above the animated foot the ground may be") f32 max_step = 0.5f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 0.5, Tooltip = "Ankle height above the ground") f32 foot_height = 0.08f;
};

// Turns the object at a steady rate while playing (Unreal's Rotating Movement).
LSD_STRUCT(Component, Category = "Movement", Tooltip = "Spins the object at a steady rate (fans, coins, wheels)")
struct RotatingMovement {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "deg/s", Tooltip = "Spin speed around each axis (Z turns it like a fan)") Vec3 rate{0.0f, 0.0f, 90.0f};
};

// Flies the object from its start velocity, pulled down by gravity, until it hits something
// (Unreal's Projectile Movement): it stops there and both get on_hit.
LSD_STRUCT(Component, Category = "Movement", Tooltip = "Flies like a projectile: speed, gravity, bounces, hits")
struct ProjectileMovement {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m/s", Tooltip = "Start velocity along the object's own axes (+X forward)") Vec3 velocity{20.0f, 0.0f, 0.0f};
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Times the world's gravity; 0 = flies straight") f32 gravity_scale = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Points along its flight (arrows, rockets)") bool rotation_follows_velocity = true;
    LSD_PROPERTY(Edit, Tooltip = "Destroyed when it hits something") bool destroy_on_hit = false;
};

// Rules of a game (Unreal's Game Mode), a data asset picked in World Settings: who the player is
// and a script that runs for the whole game.
LSD_STRUCT(Category = "Game")
struct GameMode {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Prefab", Tooltip = "Spawned at the Player Start when the level has no Character; should contain one") char player_prefab[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "Runs on an object named GameMode for the whole game") char script[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "Runs on an object named PlayerController: the player's input and menus, apart from the body it moves (lsd.player())")
    char player_controller[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:UI", Tooltip = "A UI document (.rml) shown when the game starts: the HUD or a main menu") char hud[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Show the mouse cursor (a menu) instead of capturing the mouse for the player") bool show_cursor = false;
};

// One morph target's weight on a skeletal mesh (Unreal's Set Morph Target).
LSD_STRUCT()
struct MorphWeight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The shape key / blend shape name in the FBX (the Mesh Editor lists them)") char name[64] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "0 = rest, 1 = the full shape") f32 weight = 0.0f;
};

// One material slot of a mesh drawn with another material on this object only (Unreal's Override
// Materials on a component). Picked in Details > Materials, which lists the mesh's slots.
LSD_STRUCT()
struct MaterialOverride {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The mesh's material slot (its material's name in the file)") char slot[64] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Drawn with this instead of the slot's material") char material[33] = "";
};

// A rigged, animated mesh (Unreal's Skeletal Mesh Component in Animation Asset mode): drag a
// rigged FBX into the viewport; its animations can come from the same file or another (Mixamo).
LSD_STRUCT(Component, Category = "Animation", Tooltip = "Shows a rigged, animated mesh: a clip or an Animation Graph")
struct SkeletalMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "An FBX with a skeleton and skin") char mesh[33] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Auto,+X,+Y,-X,-Y",
                 Tooltip = "Which way the character's front points in its file; it is turned to face the object's forward (+X, where the Character walks and the camera looks). Auto: Unreal rigs (UE4 / UE5 exports face -Y here, Unreal's Blueprint turns them -90) are turned; others face +X")
    u32 facing = 0;
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "One material for the whole mesh, over its material slots (empty: each slot's own material, see Materials below)") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour of the mesh where no material is set") LinearColor color = LinearColor::hex(0xB4B4B8);
    // Edited in Details > Materials (one row per slot of the mesh), so saved but not shown as a list.
    LSD_PROPERTY(Save, Tooltip = "Material slots drawn with another material on this object (Unreal's Override Materials)") List<MaterialOverride, 48> material_overrides;
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The FBX holding the animation, matched by bone names; empty = the mesh's own file") char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation (take) in that file, from 0") u32 clip = 0;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Playback speed of the animation (1 = as made)") f32 play_rate = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "The clip starts over when it ends") bool looping = true;
    LSD_PROPERTY(Edit, Tooltip = "Plays in the editor too, so you see the motion while building") bool playing = true;
    LSD_PROPERTY(Edit, Tooltip = "Root Motion (Unreal's Root Motion from Everything): in Play the animation's root travel moves the object (a Character through its capsule) instead of sliding the mesh away; the editor shows it in place")
    bool root_motion = false;
    LSD_PROPERTY(Edit, Widget = "Asset:AnimGraph", Tooltip = "A state machine (Idle, Walk/Run blend space, Jump...) driven by parameters; replaces the clip above")
    char anim_graph[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Morph targets (blend shapes: faces, correctives) mixed in by weight; scripts set them with lsd.set_morph")
    List<MorphWeight, 64> morphs;
    LSD_PROPERTY(Edit, Tooltip = "Copies the pose of this object's Skeletal Mesh bone by bone, by bone name (Unreal's Leader Pose Component): clothes and hair skinned to the same skeleton as a body. Empty: its own animation")
    char leader_pose[64] = "";
};

// One body area of an Outfit: what is worn there and the bone a rigid piece sticks to.
LSD_STRUCT(Category = "Character")
struct OutfitSlot {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The body area (Shirt, Pants, Shoes, Gloves, Hat...); scripts name it in lsd.set_outfit") char slot[24] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "What is worn there: a mesh skinned to the body's skeleton follows its pose; a rigid mesh (a hat, glasses) sticks to Bone. Empty: nothing")
    char mesh[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "The bone (or socket) a rigid piece sticks to (UE5 Manny names: head, spine_03, hand_r, pelvis, foot_r...)") char bone[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Tint of the piece") LinearColor color = LinearColor::hex(0xFFFFFF);
};

// Modular clothes (Unreal's modular characters with a Leader Pose): each slot's mesh becomes a child
// of this object, skinned pieces copying its skeleton's pose by bone name, rigid ones attached to
// their area's bone. An empty list gets the UE5 Manny areas.
LSD_STRUCT(Component, Category = "Character", Tooltip = "Clothes and gear by body area (shirt on the chest, gloves on the hands...): they follow the Skeletal Mesh's skeleton")
struct Outfit {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The body areas and what is worn on each") List<OutfitSlot, 16> slots;
};

// A child the Outfit made for one of its slots (replaced when the slot changes).
LSD_STRUCT(Component, Category = "Character", Tooltip = "Made by an Outfit for one of its slots")
struct OutfitPiece {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The Outfit slot it shows") char slot[24] = "";
};

// Gameplay tags (Unreal's Gameplay Tags): dotted names, so "Damage.Fire" also counts as "Damage".
// Picked from the project's tag list (Project Settings > Gameplay Tags) or typed. Any data asset field
// with Widget = "Tags" gets the same picker (a tag container).
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Labels scripts check (Has Tag node, lsd.has_tag): Enemy, Pickup...")
struct Tags {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Tags", Tooltip = "Comma-separated, e.g. Enemy, Damage.Fire") char tags[256] = "";
};

// Runs a Lua file in Play: on_start(self) once, on_update(self, dt) every frame (see engine/script).
// One per-object value of a script's exposed property (see Script).
LSD_STRUCT()
struct ScriptProperty {
    LSD_GENERATED()
    LSD_PROPERTY(Save) char name[32] = "";
    LSD_PROPERTY(Save) char value[64] = "";  // as text: a number, true / false, or a string
};

LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Runs a Lua, JavaScript or visual script on this object in Play")
struct Script {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "The Lua, JavaScript or visual script that runs on this object in Play") char script[33] = "";
    // This object's values for the script's `properties = { speed = 5, ... }` (Unreal's Instance
    // Editable variables): shown in Details under the script, set on self before on_start.
    LSD_PROPERTY(Save) List<ScriptProperty, 16> properties;
};

// A C or C++ script from the project's game code (Source/ -> Game.dll, lsd_game.h): the same events
// as a Lua Script, compiled (Unreal: an Actor Component class from your C++ module).
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Runs a C / C++ script from the game code (Game.dll) on this object")
struct NativeScript {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "NativeScript", Tooltip = "A C / C++ script from Source/ (Tools > New C / C++ Script makes one, Compile Game Code builds them)") char script[64] = "";
    // This object's values for the script's properties (shown in Details under it).
    LSD_PROPERTY(Save) List<ScriptProperty, 16> properties;
};

// A curve through points (Unreal's Spline Component): paths for platforms, enemies and cameras.
// Select the object to see its points in the viewport; click one to move it with the gizmo.
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "A curve shaped by points: paths, rails, routes for movers and AI")
struct Spline {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Points the curve passes through, in this object's local space") List<Vec3, 32> points = {2, {Vec3{0, 0, 0}, Vec3{4, 0, 0}}};
    LSD_PROPERTY(Edit, Tooltip = "Joins the last point back to the first") bool closed = false;
    LSD_PROPERTY(Edit, Tooltip = "Per point (same order), local: the curve's direction and pull there. (0, 0, 0) or missing = automatic. Drag the handle of a selected point")
    List<Vec3, 32> tangents;
};

// The Spline's tangents as an array lined up with its points (missing ones automatic): pass to the spline_* math.
inline void spline_tangents(const Spline& s, Vec3 out[32]) {
    for (u32 i = 0; i < 32; ++i) out[i] = i < s.tangents.count ? s.tangents[i] : Vec3{};
}

// Copies of a mesh placed along this object's Spline (Unreal's spline-placed meshes: fences, rails,
// lamp posts). They are kept in an Instanced Static Mesh on the same object, rebuilt when the spline changes.
LSD_STRUCT(Component, Category = "Rendering", Tooltip = "Repeats a mesh along the object's Spline (fences, rails, pipes)")
struct SplineMeshes {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The mesh repeated along the spline (fences, rails, pipes)") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Material for the repeated meshes") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Colour of the repeated meshes (without a material)") LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Tooltip = "Distance between copies along the curve") f32 spacing = 2.0f;
    LSD_PROPERTY(Edit, Tooltip = "Turn each copy to face along the curve") bool follow_curve = true;
    LSD_PROPERTY(Edit, Tooltip = "Scale of each copy") Vec3 scale{1.0f, 1.0f, 1.0f};
    LSD_PROPERTY(Edit, Tooltip = "The repeated meshes cast shadows") bool cast_shadows = true;
};

// Moves this object along a Spline in Play. With collision it is a moving platform the player rides.
LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Travels along the object's Spline")
struct SplineFollower {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Name of the object with the Spline") char spline[64] = "";
    LSD_PROPERTY(Edit, Units = "m/s", Tooltip = "How fast it travels along the spline") f32 speed = 2.0f;
    LSD_PROPERTY(Edit, Widget = "Choice:Once,Loop,Ping-Pong", Tooltip = "Once: stops at the end. Loop: starts over. Ping-Pong: goes back and forth") u32 mode = 1;
    LSD_PROPERTY(Edit, Tooltip = "Turn to face along the curve") bool face_forward = false;
};

// One property this instance changed from its prefab (Unreal's overridden, bold, values).
LSD_STRUCT()
struct PrefabOverride {
    LSD_GENERATED()
    LSD_PROPERTY(Save) char object[64] = "";     // names below the instance root, "Body/Wheel"; "" = the root
    LSD_PROPERTY(Save) char component[32] = "";  // TypeInfo name, "StaticMesh"
    LSD_PROPERTY(Save) char property[32] = "";   // top-level property, "color"; "" for the structural kinds
    // 0 a changed property; 1 a component the instance added; 2 one it removed; 3 a child object it
    // added (kept whole, with its children); 4 a child object it removed.
    LSD_PROPERTY(Save) u32 kind = 0;
};

// Root of a placed prefab (Unreal's Blueprint actor / Packed Level Actor). Its children and its
// other components come from the prefab asset, except the properties listed as overrides: those
// keep this instance's values when the prefab changes. Apply writes the instance back to the
// prefab; Revert drops the overrides and reloads.
LSD_STRUCT(Component, Category = "Prefab", Tooltip = "A copy of a prefab; edits to the prefab update it")
struct PrefabInstance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Prefab", Tooltip = "The prefab this object is a copy of; edits to the prefab update it") char prefab[33] = "";
    LSD_PROPERTY(Save) List<PrefabOverride, 32> overrides;
};

// ---- Lights (physical units, like Unreal). Each points along its object's forward axis (+X). ----

// The sun (Unreal's Directional Light): parallel light from far away.
LSD_STRUCT(Component, Category = "Lights", Tooltip = "Sunlight: one direction, lights everything, casts cascaded shadows")
struct DirectionalLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The sunlight's colour") LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lux", Min = 0, Tooltip = "Light on a surface facing it (10 = Unreal's default sun)") f32 intensity = 10.0f;
    LSD_PROPERTY(Edit, Tooltip = "Use the colour temperature instead of the colour") bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000, Tooltip = "1900 candle, 3200 tungsten, 5500 noon sun, 6500 overcast") f32 temperature = 6500.0f;
    LSD_PROPERTY(Edit, Tooltip = "The first sun with this on casts shadows (4 cascades)") bool cast_shadows = true;
    LSD_PROPERTY(Edit, Units = "m", Min = 5, Max = 1000, Tooltip = "Shadows end this far from the camera; shorter is sharper") f32 shadow_distance = 60.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 2,
                 Tooltip = "Contact Shadow Length (Unreal's): a short screen-space shadow ray this long toward the sun, for the small contact shadows the shadow map misses (feet on the floor, props on a table); 0 = off. Needs a Post Process Volume's look (the default one counts)")
    f32 contact_shadow_length = 0.0f;
};

// Unreal's Sphere Reflection Capture: what is around this point, captured from here, is what shiny
// surfaces inside Radius reflect (instead of the sky). Captured when the level opens and on Build >
// Build Reflection Captures (move things, then build again); up to 8 in a level.
LSD_STRUCT(Component, Category = "Lights", Tooltip = "Shiny surfaces inside its radius reflect what is around it (Sphere Reflection Capture); Build > Build Reflection Captures after changes")
struct ReflectionCapture {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0.5, Max = 500, Tooltip = "Surfaces this close reflect this capture (it fades out over the last quarter)") f32 radius = 10.0f;
};

// A bulb (Unreal's Point Light): shines in every direction.
LSD_STRUCT(Component, Category = "Lights", Tooltip = "A bulb: light in every direction from a point")
struct PointLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The light's colour") LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lm", Min = 0, Tooltip = "Total light: 800 is a household bulb") f32 intensity = 1000.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.01, Tooltip = "No light beyond this; smaller is cheaper") f32 attenuation_radius = 10.0f;
    LSD_PROPERTY(Edit, Tooltip = "Colour from Temperature instead of Color (warm bulbs, cold daylight)") bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000, Tooltip = "Colour temperature: 2000 K candle, 3200 K bulb, 6500 K daylight") f32 temperature = 3200.0f;
    LSD_PROPERTY(Edit, Tooltip = "Uses 6 of the 16 shadow tiles; the lights nearest the camera get them first") bool cast_shadows = true;
};

// A cone of light (Unreal's Spot Light): torches, street lamps, stage lights.
LSD_STRUCT(Component, Category = "Lights", Tooltip = "A cone of light (torches, street lamps)")
struct SpotLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The light's colour") LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lm", Min = 0, Tooltip = "Brightness (a 100 W bulb is about 1600 lumens)") f32 intensity = 1000.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.01, Tooltip = "How far the light reaches") f32 attenuation_radius = 10.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89, Tooltip = "Full brightness inside this angle from the centre") f32 inner_angle = 20.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 1, Max = 89, Tooltip = "No light outside this angle from the centre") f32 outer_angle = 35.0f;
    LSD_PROPERTY(Edit, Tooltip = "Colour from Temperature instead of Color") bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000, Tooltip = "Colour temperature: 2000 K candle, 3200 K bulb, 6500 K daylight") f32 temperature = 3200.0f;
    LSD_PROPERTY(Edit, Tooltip = "Uses 1 of the 16 shadow tiles; the lights nearest the camera get them first") bool cast_shadows = true;
};

// Ambient light from the sky and the ground (Unreal's Sky Light, without a captured cubemap yet).
LSD_STRUCT(Component, Category = "Lights", Tooltip = "Soft ambient light from the sky and the ground")
struct SkyLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Ambient light from above") LinearColor sky_color = LinearColor::hex(0x4D5C73);
    LSD_PROPERTY(Edit, Tooltip = "Ambient light bounced from the ground") LinearColor ground_color = LinearColor::hex(0x292624);
    LSD_PROPERTY(Edit, Min = 0, Max = 20, Tooltip = "1 = the editor's default ambient") f32 intensity = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Take the colours from the level's Sky (Unreal's Sky Light capturing the sky): sunsets light the level orange, night dark. Untick to use the colours above") bool capture_sky = true;
};

// The sky behind everything (Unreal's Sky Atmosphere, as a gradient): colours as they appear, a sun
// disc in the first Directional Light's direction. Without one the viewport keeps its plain colour.
LSD_STRUCT(Component, Category = "Environment", Tooltip = "The sky gradient and the sun disc, following the Directional Light")
struct Sky {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Colours from the sun's height (Unreal's Sky Atmosphere): blue day, orange sunrise / sunset, dark night; turn the Directional Light to change the time of day. Untick to use the colours below") bool atmosphere = true;
    LSD_PROPERTY(Edit, Tooltip = "Straight up") LinearColor zenith = LinearColor::hex(0x2F6FC4);
    LSD_PROPERTY(Edit, Tooltip = "At the horizon (match the fog colour for a seamless distance)") LinearColor horizon = LinearColor::hex(0xB8D2EA);
    LSD_PROPERTY(Edit, Tooltip = "Below the horizon") LinearColor ground = LinearColor::hex(0x4A4640);
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How bright the sky is") f32 brightness = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Draw the sun where the Directional Light shines from") bool sun_disc = true;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0.1, Max = 10, Tooltip = "How big the sun disc looks") f32 sun_size = 0.8f;
    LSD_PROPERTY(Edit, Tooltip = "Colour of the sun disc") LinearColor sun_color = LinearColor::hex(0xFFF2D6);
};

// Exponential height fog (Unreal's Exponential Height Fog): thicker low down, thinning with height.
LSD_STRUCT(Component, Category = "Environment", Tooltip = "Fog that thickens lower down and with distance")
struct HeightFog {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How thick at the fog's height (per metre)") f32 density = 0.01f;
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "How fast it thins going up (per metre); 0 = the same everywhere") f32 height_falloff = 0.2f;
    LSD_PROPERTY(Edit, Tooltip = "Colour of the fog") LinearColor color = LinearColor::hex(0xB8D2EA);
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "No fog nearer than this") f32 start_distance = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Never thicker than this (1 = can hide everything)") f32 max_opacity = 1.0f;
    LSD_PROPERTY(Edit, Category = "Volumetric", Tooltip = "Volumetric fog (Unreal's): the sun lights the fog and shadows cut through it (light shafts, god rays). Needs a Directional Light that casts shadows")
    bool volumetric = false;
    LSD_PROPERTY(Edit, Category = "Volumetric", Min = 0, Max = 1, Tooltip = "How strongly light and shadow shape the fog (0 = plain height fog)") f32 scattering = 0.8f;
    LSD_PROPERTY(Edit, Category = "Volumetric", Min = -0.9, Max = 0.9, Tooltip = "How much the light scatters forward: higher glows brighter looking toward the sun") f32 anisotropy = 0.5f;
    LSD_PROPERTY(Edit, Category = "Volumetric", Units = "m", Min = 1, Tooltip = "How far from the camera shafts are traced") f32 view_distance = 60.0f;
    LSD_PROPERTY(Edit, Category = "Volumetric", Min = 4, Max = 64, Tooltip = "Samples along each view ray: more is smoother and slower") u32 steps = 16;
};

// Colour grading and camera effects (Unreal's Post Process Volume): applies while the camera is
// inside its box (the unit cube scaled by the transform, like the other volumes) or everywhere if
// Unbound; the highest Priority wins.
// Where Build Lighting puts its light probes (Unreal's Lightmass Importance Volume): the unit cube
// scaled by the transform. Without one, probes cover all the static meshes.
LSD_STRUCT(Component, Category = "Environment", Tooltip = "Where Build Lighting bakes light probes in detail")
struct LightmassImportanceVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 20, Tooltip = "Distance between light probes; 0 = as fine as the probe budget (512) allows") f32 probe_spacing = 0.0f;
};

LSD_STRUCT(Component, Category = "Environment", Tooltip = "The look inside its box (or everywhere): exposure, bloom, grading, depth of field, SSAO")
struct PostProcessVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Everywhere, not just inside the box") bool unbound = true;
    LSD_PROPERTY(Edit, Tooltip = "Where volumes overlap, the higher priority wins") i32 priority = 0;
    LSD_PROPERTY(Edit, Units = "EV", Min = -10, Max = 10, Tooltip = "Brighter (+) or darker (-) on top of World Settings exposure") f32 exposure_compensation = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Filmic (ACES) curve: soft highlights instead of clipping") bool filmic_tonemapper = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "0 = black and white") f32 saturation = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "Difference between dark and bright (1 = unchanged)") f32 contrast = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Multiplies the image (colour grading gain)") LinearColor tint = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Darker corners") f32 vignette = 0.3f;
    LSD_PROPERTY(Edit, Widget = "Asset:Texture",
                 Tooltip = "Color Grading LUT (Unreal's): a 256 x 16 strip of 16 tiles, red across, green down, blue tile by tile. Make one by grading a screenshot with the neutral strip pasted in (Photoshop, GIMP...) and cropping the strip out; empty = none")
    char color_grading_lut[33] = "";
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Motion Blur Amount (Unreal's): the camera's own movement blurs the picture along it; 0 = off") f32 motion_blur_amount = 0.0f;
    LSD_PROPERTY(Edit, Units = "%", Min = 0, Max = 20, Tooltip = "Motion Blur Max: the longest smear, as a percentage of the screen's width") f32 motion_blur_max = 5.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How much of the LUT's look: 0 none, 1 all") f32 lut_intensity = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 4, Tooltip = "Glow around bright parts (emissive, the sun's highlights); 0 = off") f32 bloom_intensity = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "How bright (after exposure) a pixel must be to glow; 1 = white") f32 bloom_threshold = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "FXAA: smooths jagged edges") bool anti_aliasing = true;
    LSD_PROPERTY(Edit, Tooltip = "Temporal AA instead of FXAA: smoother edges and less shimmer, a little softer; can ghost on fast motion") bool temporal_aa = false;
    LSD_PROPERTY(Edit, Category = "Ambient Occlusion", Min = 0, Max = 2, Tooltip = "Darkens creases and contact points (screen space); 0 = off") f32 ao_intensity = 0.5f;
    LSD_PROPERTY(Edit, Category = "Ambient Occlusion", Units = "m", Min = 0.05, Max = 5, Tooltip = "How far around each point it looks") f32 ao_radius = 1.0f;
    LSD_PROPERTY(Edit, Category = "Reflections", Min = 0, Max = 1, Tooltip = "Screen-space reflections on smooth surfaces (low roughness); 0 = off") f32 ssr_intensity = 0.8f;
    LSD_PROPERTY(Edit, Category = "Reflections", Units = "m", Min = 1, Max = 200, Tooltip = "How far reflections look") f32 ssr_max_distance = 20.0f;
    LSD_PROPERTY(Edit, Category = "Depth of Field", Units = "m", Min = 0, Max = 1000, Tooltip = "Distance that is in focus; 0 = no depth of field") f32 dof_focal_distance = 0.0f;
    LSD_PROPERTY(Edit, Category = "Depth of Field", Units = "m", Min = 0, Max = 1000, Tooltip = "Distance either side of it that stays fully sharp") f32 dof_focal_region = 0.0f;
    LSD_PROPERTY(Edit, Category = "Depth of Field", Units = "m", Min = 0.01, Max = 1000, Tooltip = "Distance from sharp to fully blurred") f32 dof_transition = 5.0f;
    LSD_PROPERTY(Edit, Category = "Depth of Field", Min = 0, Max = 32, Tooltip = "Blur radius in pixels") f32 dof_max_blur = 8.0f;
    LSD_PROPERTY(Edit, Category = "Exposure", Tooltip = "Eye adaptation: dark views brighten and bright ones darken over a moment (Unreal's Auto Exposure)") bool auto_exposure = true;
    LSD_PROPERTY(Edit, Category = "Exposure", Units = "EV", Min = -10, Max = 0, Tooltip = "How much darker than World Settings exposure it may make the image") f32 exposure_min_ev = -4.0f;
    LSD_PROPERTY(Edit, Category = "Exposure", Units = "EV", Min = 0, Max = 10, Tooltip = "How much brighter") f32 exposure_max_ev = 4.0f;
    LSD_PROPERTY(Edit, Category = "Exposure", Min = 0.1, Max = 20, Tooltip = "How fast it brightens (walking into the dark)") f32 exposure_speed_up = 3.0f;
    LSD_PROPERTY(Edit, Category = "Exposure", Min = 0.1, Max = 20, Tooltip = "How fast it darkens (walking into the sun)") f32 exposure_speed_down = 1.0f;
};

// Where the navmesh is built (Unreal's Nav Mesh Bounds Volume): the unit cube scaled by the
// transform, like the other volumes. Without any, the whole level is used.
LSD_STRUCT(Component, Category = "AI", Tooltip = "Where AI navigation is built (P shows it)")
struct NavMeshBounds {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: ignored when building the navmesh") bool enabled = true;
};

// Cuts its box out of the navmesh (Unreal's Nav Modifier Volume with the Null area): agents path
// around it though nothing blocks there (a no-go zone, a hole the player can cross but AI must not).
LSD_STRUCT(Component, Category = "AI", Tooltip = "AI paths go round its box")
struct NavModifierVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: AI may walk through its box again") bool enabled = true;
};

// One rule of a Cull Distance Volume: objects up to `size` across are hidden beyond `distance`.
LSD_STRUCT()
struct CullDistance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Objects up to this big (bounding sphere diameter) use this rule") f32 size = 1.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Hidden farther than this from the camera; 0 = never") f32 distance = 50.0f;
};

// Hides static meshes inside its box once the camera is far enough away for their size (Unreal's
// Cull Distance Volume): pebbles vanish sooner than houses. Each object uses the rule with the
// smallest Size that still fits it (the biggest rule for anything larger); a Static Mesh's own Max
// Draw Distance still applies when it is nearer.
LSD_STRUCT(Component, Category = "Volumes", Tooltip = "Hides small meshes inside it when far away")
struct CullDistanceVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Off: no culling from this volume") bool enabled = true;
    LSD_PROPERTY(Edit, Tooltip = "Rules: meshes up to Size are hidden beyond Distance (small things vanish sooner)") List<CullDistance, 8> cull_distances = {3, {{0.5f, 30.0f}, {2.0f, 80.0f}, {10.0f, 250.0f}}};
};

// Walks on the navmesh in Play, steering round other agents (an AI-controlled pawn): Lua
// lsd.move_to(e, x, y, z) or the AI Move To node sends it somewhere.
LSD_STRUCT(Component, Category = "AI", Tooltip = "An AI walker: paths around the level, avoids others")
struct NavAgent {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0, Tooltip = "Fastest walking speed of this AI") f32 max_speed = 3.5f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Tooltip = "How much room it needs (keeps it off walls and others)") f32 radius = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.1, Tooltip = "How tall it is (low ceilings it cannot pass)") f32 height = 1.8f;
    LSD_PROPERTY(Edit, Tooltip = "Turn to face where it walks") bool face_movement = true;
};

// Thinks for a Nav Agent (Unreal's AI Controller): runs a Behavior Tree with its own blackboard,
// and sees / hears the player. Perception sets CanSeePlayer (1 while in view), PlayerLocation (where
// it was last seen) and NoiseLocation (lsd.report_noise / Report Noise within hearing range); with a
// Team, CanSeeEnemy / EnemyLocation for agents of other teams; memories fade after Memory Seconds.
LSD_STRUCT(Component, Category = "AI", Tooltip = "An AI brain: a Behavior Tree, sight and hearing")
struct AIController {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:BehaviorTree", Tooltip = "The Behavior Tree that decides what it does") char behavior_tree[33] = "";
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "How far it sees the player (CanSeePlayer)") f32 sight_range = 15.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 360, Tooltip = "Field of view, centred on the object's +X") f32 sight_angle = 120.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "How far it hears noises (lsd.report_noise / Report Noise)") f32 hearing_range = 12.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0,
                 Tooltip = "Memory (Unreal's Max Age): PlayerLocation, EnemyLocation and NoiseLocation / HeardNoise are forgotten this long after it last saw or heard them (0 = never)")
    f32 memory_seconds = 10.0f;
    LSD_PROPERTY(Edit, Tooltip = "Team (0 = none): it sees agents of other teams as enemies (CanSeeEnemy, EnemyLocation); guards and monsters on different teams fight")
    u32 team = 0;
    LSD_PROPERTY(Edit, Tooltip = "The player's team: an agent on this team (Team not 0) is friendly and ignores the player (no CanSeePlayer)") u32 player_team = 1;
};

// How the navmesh is built (Unreal's Project Settings > Navigation Mesh, per level here).
LSD_STRUCT()
struct NavSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Tooltip = "How far from walls agents keep") f32 agent_radius = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.2, Tooltip = "Height of the walkers the navmesh is built for") f32 agent_height = 1.8f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Tallest step walkers climb") f32 max_step_height = 0.45f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89, Tooltip = "Steepest ground walkers go up") f32 max_slope = 45.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Max = 1, Tooltip = "Smaller: follows the geometry closer, builds slower") f32 cell_size = 0.15f;
};

// Per-level settings (Unreal's World Settings), saved in the level file.
LSD_STRUCT(Category = "World")
struct WorldSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Falling below this height respawns the player") f32 kill_z = -50.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0, Tooltip = "Downward pull: 9.81 is Earth, 1.62 the Moon") f32 gravity = 9.81f;
    LSD_PROPERTY(Edit, Min = -10, Max = 20, Tooltip = "Fixed camera exposure (EV100) once the level has lights: higher is darker. 1.5 suits the default 10 lux sun; a 100000 lux real sun wants about 15")
    f32 exposure_ev100 = 1.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:GameMode", Tooltip = "Rules for Play: player prefab and game script") char game_mode[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "This level's own script (Unreal's Level Blueprint): a visual script or Lua file for one-off events; it runs while the level plays") char level_script[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "How the navmesh is built for AI (agent size, steps, slopes)") NavSettings navigation;
    LSD_PROPERTY(Edit, Widget = "Choice:Off,256,512,1024,2048,4096", Tooltip = "Build Lighting's lightmap atlas for the level's static meshes (sharp baked shadows and bounced light); Off: light probes only")
    u32 lightmap_resolution = 3;
};

} // namespace lsd::world
