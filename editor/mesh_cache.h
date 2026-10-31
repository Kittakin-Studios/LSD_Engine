#pragma once
// GPU meshes for StaticMesh components, keyed by the component's mesh string: a mesh asset's
// GUID or a basic shape name (Cube, Sphere, Plane). Loaded on first use, hot-reloaded when the
// source file or its import settings change.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

struct CachedMesh {
    static constexpr u32 kMaxLods = 4;  // LOD 0 is `gpu`; the others share its vertices
    char key[40];
    render::GpuMesh gpu;
    rhi::BufferHandle lod_indices[kMaxLods];  // [0] unused
    u32 lod_index_count[kMaxLods];
    u32 lod_count;                            // 1 = no simpler LODs
    // Which LOD to draw from how big the mesh looks: its bounding radius over its distance.
    u32 pick_lod(f32 radius_over_distance, i32 force_lod) const;
    Vec3 bounds_min, bounds_max;  // local space
    i64 compiled_time;
    bool failed;  // asset missing or import failed: drawn as nothing, retried on hot_reload
};

class MeshCache {
public:
    MeshCache(rhi::Device& device, Allocator& allocator, asset::AssetRegistry& registry, const char* data_dir);
    ~MeshCache();
    MeshCache(const MeshCache&) = delete;
    MeshCache& operator=(const MeshCache&) = delete;

    // nullptr if the mesh cannot be loaded.
    const CachedMesh* get(const char* key);
    void hot_reload();

    // The mesh's positions and triangles on the CPU, for mesh and convex collision; read on first
    // ask and kept until the next hot reload. Null if the mesh cannot be loaded.
    struct Triangles {
        std::vector<Vec3> points;
        std::vector<u32> indices;
    };
    const Triangles* triangles(const char* key);

private:
    bool load(CachedMesh& entry);
    std::unordered_map<std::string, Triangles> triangles_;  // shortcut: std containers, editor-time collision data

    rhi::Device& device_;
    Allocator& allocator_;
    asset::AssetRegistry& registry_;
    char data_dir_[256];
    Array<CachedMesh> meshes_;
};

} // namespace lsd::editor
