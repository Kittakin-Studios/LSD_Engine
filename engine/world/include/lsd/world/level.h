#pragma once
// Level files (.lsdlevel): the world as JSON text, one object per entity with its components,
// so levels diff and merge in source control. Undo snapshots and Play-in-Editor copies use the
// same text, so everything the editor can change round-trips through one format.
//
//   {
//     "lsd_level": 1,
//     "world_settings": {"kill_z": -50, "gravity": 9.81},
//     "entities": [
//       {"id": 0, "name": "Floor", "parent": -1,
//        "Transform": {...}, "StaticMesh": {...}}
//     ]
//   }
//
// "id" is the entity handle, kept on load so references (parents, selection) survive.
#include "lsd/foundation/array.h"
#include "lsd/world/world.h"

namespace lsd::world {

constexpr char kLevelExtension[] = ".lsdlevel";

// Replaces `out` with the level text (no NUL).
void save_level(const World& world, Array<char>& out);
// Clears the world and loads the text. On a syntax error the world is left empty and false is returned.
bool load_level(World& world, const char* text, usize length);

bool save_level_file(const World& world, const char* path);
bool load_level_file(World& world, const char* path);

} // namespace lsd::world
