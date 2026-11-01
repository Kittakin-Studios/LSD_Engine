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

// Makes `instance` match the prefab text, keeping its overridden properties. False (instance
// untouched) if the text holds no root object.
// `new_placement` also takes the prefab root's rotation and scale (when it is first placed).
bool update_instance(World& world, Entity instance, const char* text, usize length, bool new_placement = false);
bool update_instance_from_file(World& world, Entity instance, const char* path, bool new_placement = false);

// ---- Overrides ----
// Path of `object` below the instance root ("" for the root itself); false if it is not inside it.
bool instance_path(const World& world, Entity root, Entity object, char* out, usize capacity);
Entity find_in_instance(const World& world, Entity root, const char* path);  // invalid if missing
bool is_overridden(const World& world, Entity root, const char* path, const char* component, const char* property);
// Marks (or unmarks) a property as this instance's own. False if the instance's list is full.
bool set_override(World& world, Entity root, const char* path, const char* component, const char* property, bool on);
// Compares the instance with its prefab and lists what it added or removed (components, child
// objects) as structural overrides, replacing the previous ones; those changes then survive prefab
// updates. Call after an edit. False if the list filled up (the rest are not kept).
bool record_structure_overrides(World& world, Entity root, const char* prefab_text, usize length);

// Nested prefabs: update_instance also refreshes prefab instances inside the instance from their own
// files, found through this (a prefab GUID to its file path). Without it they keep the outer prefab's copy.
using PrefabPathFn = bool (*)(void* user, const char* guid, char* out, usize capacity);
void set_prefab_resolver(PrefabPathFn resolve, void* user);

} // namespace lsd::world
