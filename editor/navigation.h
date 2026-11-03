#pragma once
// The level's navmesh: built from what the player collides with (meshes with collision, Blocking
// Volumes: the same boxes physics uses) inside the Nav Mesh Bounds volumes (all of it if there are none).
#include "mesh_cache.h"

#include "lsd/ai/navigation.h"
#include "lsd/world/world.h"

namespace lsd::editor {

// False (and an empty mesh) if the level has nothing to walk on.
// `input_hash`: when given and the level's navigation inputs hash the same as last time (and `out`
// still holds that mesh), the build is skipped; it is updated after a build. `reused` says which.
bool build_level_navmesh(const world::World& world, MeshCache& meshes, ai::NavMesh& out, u64* input_hash = nullptr, bool* reused = nullptr);

} // namespace lsd::editor
