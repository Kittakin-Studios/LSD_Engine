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
    LSD_PROPERTY(Edit, Widget = "Choice:Box,Sphere,Capsule", Tooltip = "Collision shape, fitted to the mesh bounds") u32 shape = 0;
    LSD_PROPERTY(Edit, Units = "kg", Min = 0.01) f32 mass = 10.0f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 friction = 0.5f;
    LSD_PROPERTY(Edit, Min = 0, Max = 1) f32 bounciness = 0.0f;
};

// Holds this RigidBody to another object or to the world (Unreal's Physics Constraint): doors,
// chains, pendulums, drawers, fans. Pivot and axis are in this object's local space (mesh units,
// so (0, -0.5, 0) is the edge of a cube); the yellow marker in the viewport shows them.
LSD_STRUCT(Component, Category = "Physics")
struct PhysicsJoint {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Fixed,Hinge,Ball,Slider", Tooltip = "Fixed: welded. Hinge: turns about the axis. Ball: turns any way. Slider: moves along the axis") u32 type = 1;
    LSD_PROPERTY(Edit, Tooltip = "Name of the object to hold on to; empty = the world") char other[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "Where it turns, local") Vec3 pivot;
    LSD_PROPERTY(Edit, Tooltip = "Hinge or slider axis, local") Vec3 axis = {0, 0, 1};
    LSD_PROPERTY(Edit) bool limited = false;
    LSD_PROPERTY(Edit, Tooltip = "Degrees (hinge) or metres (slider) from where it starts") f32 limit_min = -45.0f;
    LSD_PROPERTY(Edit, Tooltip = "Degrees (hinge) or metres (slider) from where it starts") f32 limit_max = 45.0f;
    LSD_PROPERTY(Edit, Tooltip = "Hinge deg/s or slider m/s; 0 = no motor") f32 motor_speed = 0.0f;
    LSD_PROPERTY(Edit, Min = 0, Tooltip = "N m (hinge) or N (slider)") f32 motor_strength = 1000.0f;
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
};

// Rules of a game (Unreal's Game Mode), a data asset picked in World Settings: who the player is
// and a script that runs for the whole game.
LSD_STRUCT(Category = "Game")
struct GameMode {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Prefab", Tooltip = "Spawned at the Player Start when the level has no Character; should contain one") char player_prefab[33] = "";
    LSD_PROPERTY(Edit, Widget = "Asset:Script", Tooltip = "Runs on an object named GameMode for the whole game") char script[33] = "";
};

// Gameplay tags (Unreal's Gameplay Tags): dotted names, so "Damage.Fire" also counts as "Damage".
// shortcut: free text, no project-wide tag list or picker yet; add one with the tag editor in Project Settings.
LSD_STRUCT(Component, Category = "Gameplay")
struct Tags {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "Comma-separated, e.g. Enemy, Damage.Fire") char tags[256] = "";
};

// Runs a Lua file in Play: on_start(self) once, on_update(self, dt) every frame (see engine/script).
LSD_STRUCT(Component, Category = "Gameplay")
struct Script {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:Script") char script[33] = "";
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

// Per-level settings (Unreal's World Settings), saved in the level file.
LSD_STRUCT(Category = "World")
struct WorldSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Falling below this height respawns the player") f32 kill_z = -50.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0) f32 gravity = 9.81f;
    LSD_PROPERTY(Edit, Widget = "Asset:GameMode", Tooltip = "Rules for Play: player prefab and game script") char game_mode[33] = "";
};

} // namespace lsd::world
