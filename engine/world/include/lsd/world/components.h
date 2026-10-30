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
    LSD_PROPERTY(Edit) LinearColor color = LinearColor::hex(0xB4B4B8);
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

// Where Play puts the player; facing = the entity's Z rotation.
LSD_STRUCT(Component, Category = "Gameplay")
struct PlayerStart {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Min = 0) f32 eye_height = 1.7f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 walk_speed = 5.0f;
    LSD_PROPERTY(Edit, Units = "m/s", Min = 0) f32 jump_speed = 5.0f;
};

// Per-level settings (Unreal's World Settings), saved in the level file.
LSD_STRUCT(Category = "World")
struct WorldSettings {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Units = "m", Tooltip = "Falling below this height respawns the player") f32 kill_z = -50.0f;
    LSD_PROPERTY(Edit, Units = "m/s2", Min = 0) f32 gravity = 9.81f;
};

} // namespace lsd::world
