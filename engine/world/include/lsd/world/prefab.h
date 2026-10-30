#pragma once
// Prefabs (.lsdprefab): a root object and its children saved as level text, placed many times.
// Each placed root carries a PrefabInstance pointing at the asset; updating an instance replaces
// its children and its components (except Transform and PrefabInstance) with the prefab's, so the
// root keeps its handle, name, placement and parent.
#include "lsd/foundation/array.h"
#include "lsd/world/world.h"

namespace lsd::world {

constexpr char kPrefabExtension[] = ".lsdprefab";

// `root` and its children as text; the root is saved at the origin (its rotation and scale kept)
// and without its PrefabInstance.
void save_prefab(const World& world, Entity root, Array<char>& out);
bool save_prefab_file(const World& world, Entity root, const char* path);

// Makes `instance` match the prefab text. False (instance untouched) if the text holds no root object.
// `new_placement` also takes the prefab root's rotation and scale (when it is first placed).
bool update_instance(World& world, Entity instance, const char* text, usize length, bool new_placement = false);
bool update_instance_from_file(World& world, Entity instance, const char* path, bool new_placement = false);

} // namespace lsd::world
