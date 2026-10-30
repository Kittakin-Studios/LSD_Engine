#pragma once
// The level's navmesh: built from what the player collides with (meshes with collision, Blocking
// Volumes: the same boxes physics uses) inside the Nav Mesh Bounds volumes (all of it if there are none).
#include "mesh_cache.h"

#include "lsd/ai/navigation.h"
#include "lsd/world/world.h"

namespace lsd::editor {

// False (and an empty mesh) if the level has nothing to walk on.
bool build_level_navmesh(const world::World& world, MeshCache& meshes, ai::NavMesh& out);

} // namespace lsd::editor
