#pragma once
// GPU meshes for StaticMesh components, keyed by the component's mesh string: a mesh asset's
// GUID or a basic shape name (Cube, Sphere, Plane). Loaded on first use, hot-reloaded when the
// source file or its import settings change.
#include "key_index.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/render/lod.h"
#include "lsd/render/mesh.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lsd::editor {

struct CachedMesh {
    static constexpr u32 kMaxLods = render::kMaxLods;  // LOD 0 is `gpu`; the others share its vertices
    char key[80];  // the component's mesh, plus "|box:x,y,z" for a Box UV variant
    render::GpuMesh gpu;
    rhi::BufferHandle lod_indices[kMaxLods];  // [0] unused
    u32 lod_index_count[kMaxLods];
    u32 lod_count;                            // 1 = no simpler LODs
    // Which LOD to draw from how big the mesh looks: its bounding radius over its distance.
    u32 pick_lod(f32 radius_over_distance, i32 force_lod) const;
    f32 lod_screen_size[kMaxLods];            // [1..3]: from the asset's import settings (LOD category); [4], [5]: half and a quarter of [3]
    Vec3 bounds_min, bounds_max;  // local space
    u64 vertex_bytes, index_bytes;  // LOD 0's on the GPU, for stat dump
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

    // nullptr if the mesh cannot be loaded, or while a mesh asset compiles in the background (its first use in a
    // project, or after its file changed): it shows up once compiled, a few at a time per frame.
    const CachedMesh* get(const char* key);
    // Waits for the background compiles and loads those meshes (Play and navigation need every mesh now).
    void finish_pending();
    // A copy of `mesh` with box-projected UVs at `scale` (one texture tile per metre on every face;
    // Unreal's BSP / Modeling auto UVs), so scaled blockout shapes do not stretch their textures.
    // shortcut: one variant per distinct scale (rounded to 1 cm); fine for blockouts, not for thousands of sizes
    const CachedMesh* get_box_uv(const char* mesh, Vec3 scale);
    void hot_reload();
    // stat dump: meshes on the GPU and their bytes (index bytes include the LODs').
    struct Memory {
        u32 meshes = 0;
        u64 vertex_bytes = 0, index_bytes = 0, lod_index_bytes = 0;
        f64 load_ms = 0.0, lod_ms = 0.0;  // spent loading meshes so far, and of that building their LODs
    };
    Memory memory() const;

    // The mesh's positions and triangles on the CPU, for mesh and convex collision; read on first
    // ask and kept until the next hot reload. Null if the mesh cannot be loaded.
    struct Triangles {
        std::vector<Vec3> points;
        std::vector<u32> indices;
        std::vector<f32> uvs;  // two per point (cloth draws from them)
    };
    const Triangles* triangles(const char* key);
    // The whole vertices (normals, UVs) and triangles on the CPU, read now (not kept): fracture cuts them.
    // `stamp`: the compiled file's time (0 for shapes), so cut pieces know when the mesh changed.
    bool load_cpu(const char* key, std::vector<rhi::MeshVertex>& vertices, std::vector<u32>& indices, i64* stamp = nullptr);
    // The mesh file's collision nodes (UCX_ / UBX_ / USP_ / UCP_) as convex hulls in the mesh's space, read
    // from the source on first ask and kept until it is reimported. Null when it has none (or is a shape / part).
    struct Hulls {
        std::vector<Vec3> points;  // one hull after another
        std::vector<u32> sizes;
    };
    const Hulls* hulls(const char* key);

    // Material Slots on a Static Mesh (Unreal's per-section materials): a whole-file mesh asset whose Material Slots are
    // set draws as its parts (their own "<guid>#<k>" meshes, placed as Create Prefab from Parts places them), each with
    // its slot's Material. Null when the key is not such a mesh (a shape, a part, no slots set). Kept until hot_reload.
    // shortcut: the parts are separate meshes (more draws than one mesh with sections), sections in the compiled mesh when that matters
    struct SlotPart {
        char key[48];
        Mat4 local;          // the part in the mesh's space
        char material[33];   // its slot's Material ("" = none)
        char slot[64];       // its material's name in the file: the slot a placed object's Material Override names
    };
    // `all`: also when no slot has a Material (an object overriding slots needs the parts to draw them one by one).
    const std::vector<SlotPart>* slot_parts(const char* key, bool all = false);

    // Puts `data` under `key` (drawn, picked and collided like any mesh), replacing what was there.
    // Keys starting "Landscape:" are only ever generated: get() is quiet about them until then.
    void set_generated(const char* key, const render::MeshData& data);
    rhi::Device& device() { return device_; }
    const char* data_dir() const { return data_dir_; }
    const asset::AssetRegistry& registry() const { return registry_; }

private:
    bool load(CachedMesh& entry);
    i64 source_stamp(const char* key) const;  // the asset's source and .meta times together; 0 for shapes and generated meshes
    u32 slot_parts_generation_ = ~0u;
    std::vector<i64> stamps_;
    std::unordered_set<u32> pending_;  // meshes_ entries compiling in the background (drawn as nothing until then)
    bool finish(u32 slot);             // loads a pending entry whose compile is done; false while it still compiles
    i64 finish_window_ = 0, finish_spent_ = 0;  // steady-clock ns: loads of finished compiles spread over frames  // source_stamp of each meshes_ entry when it was last loaded: hot_reload skips the unchanged
    std::unordered_map<std::string, Triangles> triangles_;  // shortcut: std containers, editor-time collision data
    std::unordered_map<std::string, Hulls> hulls_;           // empty: the file has no collision nodes
    std::unordered_map<std::string, std::vector<SlotPart>> slot_parts_;  // empty: draws whole

    rhi::Device& device_;
    Allocator& allocator_;
    asset::AssetRegistry& registry_;
    char data_dir_[256];
    f64 load_ms_ = 0.0, lod_ms_ = 0.0;  // Memory's
    Array<CachedMesh> meshes_;  // reserved up front: pointers handed out stay valid as it fills
    KeyIndex index_;            // key -> slot in meshes_
};

} // namespace lsd::editor
