#pragma once
// Mesh import (FBX via ufbx, glTF/GLB via cgltf) and the compiled binary mesh format.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"

#include <cstddef>

namespace lsd::asset {

struct MeshBounds {
    f32 min[3] = {0, 0, 0};
    f32 max[3] = {0, 0, 0};
};

// One part of a mesh file (Unreal's import with Combine Meshes off): the triangles of one mesh node
// that use one material, plus that material's values so the editor can make a Material from them.
struct MeshPart {
    char name[64] = "";       // the node's name, plus the material's when the node has several
    char material[64] = "";   // the material's name in the file, "" = none
    f32 base_color[4] = {1, 1, 1, 1};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    char texture[128] = "";   // the base colour texture's file name without folders, "" = none
    char normal_texture[128] = "";
    char orm_texture[128] = "";  // glTF's metallic-roughness map (G roughness, B metallic, as our ORM)
    // The node's place in engine space (before the import scale: multiply `position` by it).
    Vec3 position, rotation, scale = {1, 1, 1};  // rotation: Euler degrees, as Transform
};

// Reads a source file, converts to engine space (Z up, right-handed, metres), applies `scale`,
// merges all meshes in the scene with their node transforms, welds duplicate vertices and
// orders triangles for the GPU vertex cache. Writes a reason into `error` on failure.
// `part` >= 0 imports only that part (list_mesh_parts order) in its node's own space: place it with
// the part's position / rotation / scale to line up with the others.
// Normals (Blender's Shade Smooth / Flat / Auto Smooth): 0 keeps the file's, 1 smooth (the default
// for every mesh), 2 solid (flat faces), 3 smooth except across edges sharper than `smoothing_angle`.
enum MeshNormals : u32 { kNormalsImport = 0, kNormalsSmooth = 1, kNormalsFlat = 2, kNormalsAutoSmooth = 3 };
bool import_mesh(const char* path, f32 scale, Allocator& allocator, render::MeshData& out, char* error, usize error_capacity, i32 part = -1,
                 u32 normals = kNormalsSmooth, f32 smoothing_angle = 30.0f);

// Recomputes the normals of an unindexed triangle list (3 vertices a triangle) as `normals` asks,
// for any vertex layout (positions and normals as 3 floats at those byte offsets). Smoothing follows
// the topology: corners at the same position share, whatever their UVs, so UV seams leave no
// shading seams. Area-weighted.
void compute_normals(void* triangles, usize vertex_count, usize stride, usize position_offset, usize normal_offset, u32 normals, f32 smoothing_angle,
                     Allocator& allocator);
inline void compute_normals(rhi::MeshVertex* triangles, usize vertex_count, u32 normals, f32 smoothing_angle, Allocator& allocator) {
    compute_normals(triangles, vertex_count, sizeof(rhi::MeshVertex), offsetof(rhi::MeshVertex, position), offsetof(rhi::MeshVertex, normal), normals,
                    smoothing_angle, allocator);
}

// The parts import_mesh can import one by one, in order.
bool list_mesh_parts(const char* path, Array<MeshPart>& out, char* error, usize error_capacity);

// A StaticMesh's mesh key: a mesh asset's GUID (the whole file), or "<guid>#<k>" for part k.
bool parse_mesh_key(const char* key, Guid& guid, i32& part);

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

// Data compiler step for one mesh asset: imports it if DerivedData/<guid>.lsdmesh (<guid>_<part>.lsdmesh
// for a part) is missing or stale and writes that file. `compiled_path` receives the output path.
bool compile_mesh_asset(const AssetRegistry& registry, const AssetRecord& record, const char* data_dir, Allocator& allocator,
                        char* compiled_path, usize compiled_path_capacity, i32 part = -1);

// compile_mesh_asset for a mesh key that names an asset (or a part of one); false for a basic shape.
bool compile_mesh_key(const AssetRegistry& registry, const char* key, const char* data_dir, Allocator& allocator, char* compiled_path,
                      usize compiled_path_capacity);

} // namespace lsd::asset
