#pragma once
// Mesh import (FBX via ufbx, glTF/GLB via cgltf) and the compiled binary mesh format.
#include "lsd/asset/asset_registry.h"
#include "lsd/render/mesh.h"

namespace lsd::asset {

struct MeshBounds {
    f32 min[3] = {0, 0, 0};
    f32 max[3] = {0, 0, 0};
};

// Reads a source file, converts to engine space (Z up, right-handed, metres), applies `scale`,
// merges all meshes in the scene with their node transforms, welds duplicate vertices and
// orders triangles for the GPU vertex cache. Writes a reason into `error` on failure.
// shortcut: one merged mesh per file, no materials/skinning yet (milestones 4-5).
bool import_mesh(const char* path, f32 scale, Allocator& allocator, render::MeshData& out, char* error, usize error_capacity);

MeshBounds compute_bounds(const render::MeshData& mesh);

// A simpler version of the mesh for distant LODs (Unreal's auto LOD reduction): about `ratio` of the
// triangles, sharing the mesh's vertices. Writes into `out` (room for mesh.index_count) and returns
// its index count; UV seams and borders are kept where they are.
u32 simplify_mesh(const render::MeshData& mesh, f32 ratio, u32* out);

// Compiled mesh (.lsdmesh): header + vertices + indices, loaded straight into GPU buffers.
// The header records what it was built from so stale files are rebuilt automatically.
struct CompiledMeshSource {
    u64 source_size = 0;
    i64 source_time = 0;
    u32 settings_hash = 0;
};

bool save_compiled_mesh(const char* path, const render::MeshData& mesh, const CompiledMeshSource& source);
bool load_compiled_mesh(const char* path, Allocator& allocator, render::MeshData& out, MeshBounds* bounds = nullptr);
bool compiled_mesh_up_to_date(const char* path, const CompiledMeshSource& source);

// Data compiler step for one mesh asset: imports it if DerivedData/<guid>.lsdmesh is missing or
// stale and writes that file. `compiled_path` receives the output path. Returns false on error.
bool compile_mesh_asset(const AssetRegistry& registry, const AssetRecord& record, const char* data_dir, Allocator& allocator,
                        char* compiled_path, usize compiled_path_capacity);

} // namespace lsd::asset
