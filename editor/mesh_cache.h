#pragma once
// GPU meshes for StaticMesh components, keyed by the component's mesh string: a mesh asset's
// GUID or a basic shape name (Cube, Sphere, Plane). Loaded on first use, hot-reloaded when the
// source file or its import settings change.
#include "key_index.h"
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
    char key[80];  // the component's mesh, plus "|box:x,y,z" for a Box UV variant
    render::GpuMesh gpu;
    rhi::BufferHandle lod_indices[kMaxLods];  // [0] unused
    u32 lod_index_count[kMaxLods];
    u32 lod_count;                            // 1 = no simpler LODs
    // Which LOD to draw from how big the mesh looks: its bounding radius over its distance.
    u32 pick_lod(f32 radius_over_distance, i32 force_lod) const;
    f32 lod_screen_size[kMaxLods];            // [1..3]: from the asset's import settings (LOD category)
    Vec3 bounds_min, bounds_max;  // local space
    i64 compiled_time;
    bool failed;  // asset missing or import failed: drawn as nothing, retried on hot_reload
    bool generated;  // built in code (set_generated: a landscape), never loaded from a file
};

class MeshCache {
public:
    MeshCache(rhi::Device& device, Allocator& allocator, asset::AssetRegistry& registry, const char* data_dir);
    ~MeshCache();
    MeshCache(const MeshCache&) = delete;
    MeshCache& operator=(const MeshCache&) = delete;

    // nullptr if the mesh cannot be loaded.
    const CachedMesh* get(const char* key);
    // A copy of `mesh` with box-projected UVs at `scale` (one texture tile per metre on every face;
    // Unreal's BSP / Modeling auto UVs), so scaled blockout shapes do not stretch their textures.
    // shortcut: one variant per distinct scale (rounded to 1 cm); fine for blockouts, not for thousands of sizes
    const CachedMesh* get_box_uv(const char* mesh, Vec3 scale);
    void hot_reload();

    // The mesh's positions and triangles on the CPU, for mesh and convex collision; read on first
    // ask and kept until the next hot reload. Null if the mesh cannot be loaded.
    struct Triangles {
        std::vector<Vec3> points;
        std::vector<u32> indices;
    };
    const Triangles* triangles(const char* key);

    // Material Slots on a Static Mesh (Unreal's per-section materials): a whole-file mesh asset whose Material Slots are
    // set draws as its parts (their own "<guid>#<k>" meshes, placed as Create Prefab from Parts places them), each with
    // its slot's Material. Null when the key is not such a mesh (a shape, a part, no slots set). Kept until hot_reload.
    // shortcut: the parts are separate meshes (more draws than one mesh with sections), sections in the compiled mesh when that matters
    struct SlotPart {
        char key[48];
        Mat4 local;          // the part in the mesh's space
        char material[33];   // its slot's Material ("" = none)
    };
    const std::vector<SlotPart>* slot_parts(const char* key);

    // Puts `data` under `key` (drawn, picked and collided like any mesh), replacing what was there.
    // Keys starting "Landscape:" are only ever generated: get() is quiet about them until then.
    void set_generated(const char* key, const render::MeshData& data);
    rhi::Device& device() { return device_; }
    const asset::AssetRegistry& registry() const { return registry_; }

private:
    bool load(CachedMesh& entry);
    std::unordered_map<std::string, Triangles> triangles_;  // shortcut: std containers, editor-time collision data
    std::unordered_map<std::string, std::vector<SlotPart>> slot_parts_;  // empty: draws whole

    rhi::Device& device_;
    Allocator& allocator_;
    asset::AssetRegistry& registry_;
    char data_dir_[256];
    Array<CachedMesh> meshes_;  // reserved up front: pointers handed out stay valid as it fills
    KeyIndex index_;            // key -> slot in meshes_
};

} // namespace lsd::editor
