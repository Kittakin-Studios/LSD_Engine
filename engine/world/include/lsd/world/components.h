#pragma once
// Built-in components. Each is a reflected plain struct, so the Details panel, level files, undo
// and Play-in-Editor copies all work from the same type table with no per-component code.
#include "lsd/foundation/math.h"
#include "lsd/reflect/color.h"

namespace lsd::world {

// Every entity has one (like Unreal's root scene component). Relative to the parent, if any.
LSD_STRUCT(Component, Category = "Common")
struct Transform {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg", Tooltip = "X, then Y, then Z (same as Blender's XYZ Euler)") Vec3 rotation;
    LSD_PROPERTY(Edit) Vec3 scale = {1, 1, 1};
};

LSD_STRUCT(Component, Category = "Rendering")
struct StaticMesh {
    LSD_GENERATED()
    // Edited with the editor's mesh picker (asset combo + drag-drop), so saved but not shown as text.
    LSD_PROPERTY(Save, Tooltip = "A mesh asset's GUID, or a basic shape: Cube, Sphere, Plane") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material", Tooltip = "Drag a material from the Content Browser onto the object in the viewport, or pick it here") char material[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Tints the material (white = as the material is)") LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit, Tooltip = "Blocks the player in Play") bool collision = true;
    LSD_PROPERTY(Edit, Widget = "Choice:Box,Mesh", Tooltip = "Box: the bounds (fast). Mesh: the triangles themselves (arches, ramps, stairs; Unreal's Use Complex As Simple)")
    u32 collision_shape = 0;
    LSD_PROPERTY(Edit, Widget = "Asset:PhysicalMaterial", Tooltip = "The surface's friction and bounciness") char physical_material[33] = "";
};

// Volumes are boxes the size of the entity's scale (a 1 m cube from z = 0 to 1, like the Cube shape),
// drawn only in the editor.

// Invisible wall: blocks the player (Unreal's Blocking Volume).
LSD_STRUCT(Component, Category = "Volumes")
struct BlockingVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
};

// Sends the player back to the start when entered (Unreal's Kill Z Volume).
LSD_STRUCT(Component, Category = "Volumes")
struct KillVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
};

// Tells scripts when the player or a simulated object enters or leaves it (Unreal's Trigger Box):
// on_overlap_begin(self, other) / on_overlap_end(self, other) run on both objects' scripts.
LSD_STRUCT(Component, Category = "Volumes")
struct TriggerVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
    LSD_PROPERTY(Edit, Widget = "Asset:Sound", Tooltip = "Played here when something enters (no script needed)") char enter_sound[33] = "";
};

// Changes how the world sounds while the listener is inside (Unreal's Audio Volume): its own
// reverb, and sounds on the other side of its walls muffled (inside ones heard from outside too).
LSD_STRUCT(Component, Category = "Volumes")
struct AudioVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Reverb on everything heard while inside") f32 reverb = 0.4f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 room_size = 0.7f;
    LSD_PROPERTY(Edit, Units = "Hz", Min = 100, Max = 20000, Tooltip = "Sounds through its walls are muffled to this; 20000 = not muffled") f32 muffle = 800.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "How long the reverb takes to change when entering or leaving") f32 fade = 0.5f;
};

// A sound in the level (Unreal's Ambient Sound): ambience, a river, a crackling torch. Drag a
// sound from the Content Browser into the viewport to make one.
LSD_STRUCT(Component, Category = "Audio")
struct SoundEmitter {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Sound") char sound[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Starts when Play starts; it follows the object if it moves") bool auto_play = true;
};

// A particle effect in the level (Unreal's Niagara Component): plays a Particle System data asset
// at the object. Drag a Particle System from the Content Browser into the viewport to make one.
LSD_STRUCT(Component, Category = "Effects")
struct ParticleEffect {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:ParticleSystem") char system[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Emitting; turn off (Details, Lua lsd.set) to let the particles die out") bool active = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 10, Tooltip = "Slow motion / fast forward") f32 time_scale = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Plays in the editor too (preview while placing)") bool preview = true;
};

// Where Play puts the player; facing = the entity's Z rotation.
LSD_STRUCT(Component, Category = "Gameplay")
struct PlayerStart {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 eye_height = 1.7f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 jump_speed = 5.0f;
};

// Simulated by physics in Play: falls, collides, can be pushed. The shape is fitted to the mesh's bounds.
LSD_STRUCT(Component, Category = "Physics")
struct RigidBody {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Box,Sphere,Capsule,Convex", Tooltip = "Collision shape, fitted to the mesh bounds; Convex wraps the mesh's points") u32 shape = 0;
    LSD_PROPERTY(Edit, Units = "kg", Min = 0.01) f32 mass = 10.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 friction = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 bounciness = 0.0f;
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
    LSD_PROPERTY(Edit, Min = 0, Max = 2) f32 friction = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 bounciness = 0.0f;
};

// Holds this RigidBody to another object or to the world (Unreal's Physics Constraint): doors,
// chains, pendulums, drawers, fans. Pivot and axis are in this object's local space (mesh units,
// so (0, -0.5, 0) is the edge of a cube); the yellow marker in the viewport shows them.
LSD_STRUCT(Component, Category = "Physics")
struct PhysicsJoint {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Fixed,Hinge,Ball,Slider,Cone",
                 Tooltip = "Fixed: welded. Hinge: turns about the axis. Ball: turns any way. Slider: moves along the axis. Cone: swings within Cone Angle of the axis, twist limited (ragdolls, lamps)")
    u32 type = 1;
    LSD_PROPERTY(Edit, Tooltip = "Name of the object to hold on to; empty = the world") char other[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "Where it turns, local") Vec3 pivot;
    LSD_PROPERTY(Edit, Tooltip = "Hinge or slider axis, local") Vec3 axis = {0, 0, 1};
    LSD_PROPERTY(Edit) bool limited = false;
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
LSD_STRUCT(Component, Category = "Gameplay")
struct Character {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:InputMappingContext") char mapping_context[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "Axis 2D: x = right, y = forward") char move_action[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction", Tooltip = "Axis 2D: x = turn right, y = look up") char look_action[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:InputAction") char jump_action[33] = "";
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 jump_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89) f32 max_slope = 45.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 step_height = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.2) f32 height = 1.8f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05) f32 radius = 0.35f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 eye_height = 1.65f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Tooltip = "Turn per unit of Look (per pixel for the mouse)") f32 look_sensitivity = 0.15f;
    LSD_PROPERTY(Edit) bool invert_look_y = false;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "Spring arm length behind the head: 0 = first person") f32 camera_distance = 0.0f;
    LSD_PROPERTY(Edit, Category = "Crouch", Widget = "Asset:InputAction", Tooltip = "Held: crouch (stands up again when there is room)") char crouch_action[33] = "";
    LSD_PROPERTY(Edit, Category = "Crouch", Units = "m", Min = 0.3) f32 crouch_height = 1.1f;
    LSD_PROPERTY(Edit, Category = "Crouch", Units = "m/s", Min = 0) f32 crouch_speed = 2.5f;
    LSD_PROPERTY(Edit, Category = "Swim", Units = "m/s", Min = 0, Tooltip = "Inside a Water Volume: moves where you look, Jump rises") f32 swim_speed = 3.0f;
};

// Water the Character swims in (Unreal's Physics Volume with Water Volume on): the unit cube scaled
// by the transform, like the other volumes. Swimming starts when the chest is under.
LSD_STRUCT(Component, Category = "Gameplay")
struct WaterVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
};

// A camera the game can look through (Unreal's Camera Component / Camera Actor): along its +X.
// Auto Activate: Play starts looking through it instead of the player's head; lsd.set_view(e)
// switches to any camera (0: back to the player), Timelines use them for camera cuts.
LSD_STRUCT(Component, Category = "Camera")
struct Camera {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "deg", Min = 10, Max = 170, Tooltip = "Vertical field of view") f32 fov = 60.0f;
    LSD_PROPERTY(Edit, Tooltip = "Play starts looking through this camera") bool auto_activate = false;
};

// A picture projected onto whatever is inside its box (Unreal's Decal Actor): blood, dirt, cracks,
// signs, road markings. The box is the unit cube scaled by the transform (like the volumes); it
// projects down its local -Z onto surfaces facing up its +Z. The texture's alpha cuts it out.
LSD_STRUCT(Component, Category = "Rendering")
struct Decal {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Texture") char texture[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Multiplies the texture; alpha = opacity") LinearColor tint = LinearColor::hex(0xFFFFFF);
};

// One copy in an Instanced Static Mesh, relative to the object.
LSD_STRUCT()
struct MeshInstance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m") Vec3 position;
    LSD_PROPERTY(Edit, Units = "deg") Vec3 rotation;
    LSD_PROPERTY(Edit) Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Many copies of one mesh drawn in a single call (Unreal's Instanced Static Mesh): rocks, grass,
// fences, crowds of props. Details > Scatter fills it at random over a circle.
// shortcut: no collision and no per-instance picking; built-in Materials only (a Material Graph
// draws each copy on its own).
LSD_STRUCT(Component, Category = "Rendering")
struct InstancedStaticMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh") char mesh[40] = "Cube";
    LSD_PROPERTY(Edit, Widget = "Asset:Material") char material[33] = "";
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit) bool cast_shadows = true;
    LSD_PROPERTY(Edit) List<MeshInstance, 1024> instances;
};

// Keeps this object on a socket of another object's Skeletal Mesh (Unreal's Attach to Socket): a
// sword in a hand follows the animation. The socket comes from the mesh (Mesh Editor > Sockets).
LSD_STRUCT(Component, Category = "Animation")
struct AttachToSocket {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The object with the Skeletal Mesh, by name") char target[64] = "";
    LSD_PROPERTY(Edit) char socket[32] = "";
};

// Plants a Skeletal Mesh's feet on the ground under them while playing (Unreal's foot IK): each
// foot is raised onto steps and slopes with two-bone IK. Bones by name, as in the imported skeleton.
LSD_STRUCT(Component, Category = "Animation")
struct FootIK {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) char left_foot[48] = "foot_l";
    LSD_PROPERTY(Edit) char right_foot[48] = "foot_r";
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 2, Tooltip = "How high above the animated foot the ground may be") f32 max_step = 0.5f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 0.5, Tooltip = "Ankle height above the ground") f32 foot_height = 0.08f;
};

// Turns the object at a steady rate while playing (Unreal's Rotating Movement).
LSD_STRUCT(Component, Category = "Movement")
struct RotatingMovement {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "deg/s") Vec3 rate{0.0f, 0.0f, 90.0f};
};

// Flies the object from its start velocity, pulled down by gravity, until it hits something
// (Unreal's Projectile Movement): it stops there and both get on_hit.
LSD_STRUCT(Component, Category = "Movement")
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

// A rigged, animated mesh (Unreal's Skeletal Mesh Component in Animation Asset mode): drag a
// rigged FBX into the viewport; its animations can come from the same file or another (Mixamo).
LSD_STRUCT(Component, Category = "Animation")
struct SkeletalMesh {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "An FBX with a skeleton and skin") char mesh[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Material") char material[33] = "";
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xB4B4B8);
    LSD_PROPERTY(Edit, Widget = "Asset:Mesh", Tooltip = "The FBX holding the animation, matched by bone names; empty = the mesh's own file") char animation[33] = "";
    LSD_PROPERTY(Edit, Tooltip = "Which animation (take) in that file, from 0") u32 clip = 0;
    LSD_PROPERTY(Edit, Min = 0, Max = 10) f32 play_rate = 1.0f;
    LSD_PROPERTY(Edit) bool looping = true;
    LSD_PROPERTY(Edit, Tooltip = "Plays in the editor too, so you see the motion while building") bool playing = true;
    LSD_PROPERTY(Edit, Widget = "Asset:AnimGraph", Tooltip = "A state machine (Idle, Walk/Run blend space, Jump...) driven by parameters; replaces the clip above")
    char anim_graph[33] = "";
};

// Gameplay tags (Unreal's Gameplay Tags): dotted names, so "Damage.Fire" also counts as "Damage".
// Picked from the project's tag list (Project Settings > Gameplay Tags) or typed. Any data asset field
// with Widget = "Tags" gets the same picker (a tag container).
LSD_STRUCT(Component, Category = "Gameplay")
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

LSD_STRUCT(Component, Category = "Gameplay")
struct Script {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Script") char script[33] = "";
    // This object's values for the script's `properties = { speed = 5, ... }` (Unreal's Instance
    // Editable variables): shown in Details under the script, set on self before on_start.
    LSD_PROPERTY(Save) List<ScriptProperty, 16> properties;
};

// A curve through points (Unreal's Spline Component): paths for platforms, enemies and cameras.
// Select the object to see its points in the viewport; click one to move it with the gizmo.
LSD_STRUCT(Component, Category = "Gameplay")
struct Spline {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Points the curve passes through, in this object's local space") List<Vec3, 32> points = {2, {Vec3{0, 0, 0}, Vec3{4, 0, 0}}};
    LSD_PROPERTY(Edit, Tooltip = "Joins the last point back to the first") bool closed = false;
};

// Moves this object along a Spline in Play. With collision it is a moving platform the player rides.
LSD_STRUCT(Component, Category = "Gameplay")
struct SplineFollower {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Name of the object with the Spline") char spline[64] = "";
    LSD_PROPERTY(Edit, Units = "m/s") f32 speed = 2.0f;
    LSD_PROPERTY(Edit, Widget = "Choice:Once,Loop,Ping-Pong") u32 mode = 1;
    LSD_PROPERTY(Edit, Tooltip = "Turn to face along the curve") bool face_forward = false;
};

// One property this instance changed from its prefab (Unreal's overridden, bold, values).
LSD_STRUCT()
struct PrefabOverride {
    LSD_GENERATED()
    LSD_PROPERTY(Save) char object[64] = "";     // names below the instance root, "Body/Wheel"; "" = the root
    LSD_PROPERTY(Save) char component[32] = "";  // TypeInfo name, "StaticMesh"
    LSD_PROPERTY(Save) char property[32] = "";   // top-level property, "color"
};

// Root of a placed prefab (Unreal's Blueprint actor / Packed Level Actor). Its children and its
// other components come from the prefab asset, except the properties listed as overrides: those
// keep this instance's values when the prefab changes. Apply writes the instance back to the
// prefab; Revert drops the overrides and reloads.
LSD_STRUCT(Component, Category = "Prefab")
struct PrefabInstance {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Prefab") char prefab[33] = "";
    LSD_PROPERTY(Save) List<PrefabOverride, 32> overrides;
};

// ---- Lights (physical units, like Unreal). Each points along its object's forward axis (+X). ----

// The sun (Unreal's Directional Light): parallel light from far away.
LSD_STRUCT(Component, Category = "Lights")
struct DirectionalLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lux", Min = 0, Tooltip = "Light on a surface facing it (10 = Unreal's default sun)") f32 intensity = 10.0f;
    LSD_PROPERTY(Edit, Tooltip = "Use the colour temperature instead of the colour") bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000, Tooltip = "1900 candle, 3200 tungsten, 5500 noon sun, 6500 overcast") f32 temperature = 6500.0f;
    LSD_PROPERTY(Edit, Tooltip = "The first sun with this on casts shadows (4 cascades)") bool cast_shadows = true;
    LSD_PROPERTY(Edit, Units = "m", Min = 5, Max = 1000, Tooltip = "Shadows end this far from the camera; shorter is sharper") f32 shadow_distance = 60.0f;
};

// A bulb (Unreal's Point Light): shines in every direction.
LSD_STRUCT(Component, Category = "Lights")
struct PointLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lm", Min = 0, Tooltip = "Total light: 800 is a household bulb") f32 intensity = 1000.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.01, Tooltip = "No light beyond this; smaller is cheaper") f32 attenuation_radius = 10.0f;
    LSD_PROPERTY(Edit) bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000) f32 temperature = 3200.0f;
    LSD_PROPERTY(Edit, Tooltip = "Uses 6 of the 16 shadow tiles; the lights nearest the camera get them first") bool cast_shadows = true;
};

// A cone of light (Unreal's Spot Light): torches, street lamps, stage lights.
LSD_STRUCT(Component, Category = "Lights")
struct SpotLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Units = "lm", Min = 0) f32 intensity = 1000.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.01) f32 attenuation_radius = 10.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89, Tooltip = "Full brightness inside this angle from the centre") f32 inner_angle = 20.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 1, Max = 89, Tooltip = "No light outside this angle from the centre") f32 outer_angle = 35.0f;
    LSD_PROPERTY(Edit) bool use_temperature = false;
    LSD_PROPERTY(Edit, Units = "K", Min = 1000, Max = 15000) f32 temperature = 3200.0f;
    LSD_PROPERTY(Edit, Tooltip = "Uses 1 of the 16 shadow tiles; the lights nearest the camera get them first") bool cast_shadows = true;
};

// Ambient light from the sky and the ground (Unreal's Sky Light, without a captured cubemap yet).
LSD_STRUCT(Component, Category = "Lights")
struct SkyLight {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) LinearColor sky_color = LinearColor::hex(0x4D5C73);
    LSD_PROPERTY(Edit) LinearColor ground_color = LinearColor::hex(0x292624);
    LSD_PROPERTY(Edit, Min = 0, Max = 20, Tooltip = "1 = the editor's default ambient") f32 intensity = 1.0f;
};

// The sky behind everything (Unreal's Sky Atmosphere, as a gradient): colours as they appear, a sun
// disc in the first Directional Light's direction. Without one the viewport keeps its plain colour.
LSD_STRUCT(Component, Category = "Environment")
struct Sky {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Straight up") LinearColor zenith = LinearColor::hex(0x2F6FC4);
    LSD_PROPERTY(Edit, Tooltip = "At the horizon (match the fog colour for a seamless distance)") LinearColor horizon = LinearColor::hex(0xB8D2EA);
    LSD_PROPERTY(Edit, Tooltip = "Below the horizon") LinearColor ground = LinearColor::hex(0x4A4640);
    LSD_PROPERTY(Edit, Min = 0, Max = 10) f32 brightness = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Draw the sun where the Directional Light shines from") bool sun_disc = true;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0.1, Max = 10) f32 sun_size = 0.8f;
    LSD_PROPERTY(Edit) LinearColor sun_color = LinearColor::hex(0xFFF2D6);
};

// Exponential height fog (Unreal's Exponential Height Fog): thicker low down, thinning with height.
LSD_STRUCT(Component, Category = "Environment")
struct HeightFog {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "How thick at the fog's height (per metre)") f32 density = 0.01f;
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "How fast it thins going up (per metre); 0 = the same everywhere") f32 height_falloff = 0.2f;
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xB8D2EA);
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Tooltip = "No fog nearer than this") f32 start_distance = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Never thicker than this (1 = can hide everything)") f32 max_opacity = 1.0f;
};

// Colour grading and camera effects (Unreal's Post Process Volume): applies while the camera is
// inside its box (the unit cube scaled by the transform, like the other volumes) or everywhere if
// Unbound; the highest Priority wins.
// Where Build Lighting puts its light probes (Unreal's Lightmass Importance Volume): the unit cube
// scaled by the transform. Without one, probes cover all the static meshes.
LSD_STRUCT(Component, Category = "Environment")
struct LightmassImportanceVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0, Max = 20, Tooltip = "Distance between light probes; 0 = as fine as the probe budget (512) allows") f32 probe_spacing = 0.0f;
};

LSD_STRUCT(Component, Category = "Environment")
struct PostProcessVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Everywhere, not just inside the box") bool unbound = true;
    LSD_PROPERTY(Edit) i32 priority = 0;
    LSD_PROPERTY(Edit, Units = "EV", Min = -10, Max = 10, Tooltip = "Brighter (+) or darker (-) on top of World Settings exposure") f32 exposure_compensation = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Filmic (ACES) curve: soft highlights instead of clipping") bool filmic_tonemapper = true;
    LSD_PROPERTY(Edit, Min = 0, Max = 2, Tooltip = "0 = black and white") f32 saturation = 1.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 2) f32 contrast = 1.0f;
    LSD_PROPERTY(Edit, Tooltip = "Multiplies the image (colour grading gain)") LinearColor tint = LinearColor::hex(0xFFFFFF);
    LSD_PROPERTY(Edit, Min = 0, Max = 1, Tooltip = "Darker corners") f32 vignette = 0.3f;
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
};

// Where the navmesh is built (Unreal's Nav Mesh Bounds Volume): the unit cube scaled by the
// transform, like the other volumes. Without any, the whole level is used.
LSD_STRUCT(Component, Category = "AI")
struct NavMeshBounds {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
};

// Cuts its box out of the navmesh (Unreal's Nav Modifier Volume with the Null area): agents path
// around it though nothing blocks there (a no-go zone, a hole the player can cross but AI must not).
LSD_STRUCT(Component, Category = "AI")
struct NavModifierVolume {
    LSD_GENERATED()
    LSD_PROPERTY(Edit) bool enabled = true;
};

// Walks on the navmesh in Play, steering round other agents (an AI-controlled pawn): Lua
// lsd.move_to(e, x, y, z) or the AI Move To node sends it somewhere.
LSD_STRUCT(Component, Category = "AI")
struct NavAgent {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 max_speed = 3.5f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05) f32 radius = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.1) f32 height = 1.8f;
    LSD_PROPERTY(Edit, Tooltip = "Turn to face where it walks") bool face_movement = true;
};

// Thinks for a Nav Agent (Unreal's AI Controller): runs a Behavior Tree with its own blackboard,
// and sees / hears the player. Perception sets CanSeePlayer (1 while in view), PlayerLocation (where
// it was last seen) and NoiseLocation (lsd.report_noise / Report Noise within hearing range).
LSD_STRUCT(Component, Category = "AI")
struct AIController {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:BehaviorTree") char behavior_tree[33] = "";
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 sight_range = 15.0f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 360, Tooltip = "Field of view, centred on the object's +X") f32 sight_angle = 120.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 hearing_range = 12.0f;
};

// How the navmesh is built (Unreal's Project Settings > Navigation Mesh, per level here).
LSD_STRUCT()
struct NavSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Tooltip = "How far from walls agents keep") f32 agent_radius = 0.4f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.2) f32 agent_height = 1.8f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 max_step_height = 0.45f;
    LSD_PROPERTY(Edit, Units = "deg", Min = 0, Max = 89) f32 max_slope = 45.0f;
    LSD_PROPERTY(Edit, Units = "m", Min = 0.05, Max = 1, Tooltip = "Smaller: follows the geometry closer, builds slower") f32 cell_size = 0.15f;
};

// Per-level settings (Unreal's World Settings), saved in the level file.
LSD_STRUCT(Category = "World")
struct WorldSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Falling below this height respawns the player") f32 kill_z = -50.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0) f32 gravity = 9.81f;
    LSD_PROPERTY(Edit, Min = -10, Max = 20, Tooltip = "Fixed camera exposure (EV100) once the level has lights: higher is darker. 1.5 suits the default 10 lux sun; a 100000 lux real sun wants about 15")
    f32 exposure_ev100 = 1.5f;
    LSD_PROPERTY(Edit, Widget = "Asset:GameMode", Tooltip = "Rules for Play: player prefab and game script") char game_mode[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "This level's own script (Unreal's Level Blueprint): a visual script or Lua file for one-off events; it runs while the level plays") char level_script[33] = "";
    LSD_PROPERTY(Edit) NavSettings navigation;
};

} // namespace lsd::world
