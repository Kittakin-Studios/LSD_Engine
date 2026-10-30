#pragma once
// GPU meshes for StaticMesh components, keyed by the component's mesh string: a mesh asset's
// GUID or a basic shape name (Cube, Sphere, Plane). Loaded on first use, hot-reloaded when the
// source file or its import settings change.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"

namespace lsd::editor {

struct CachedMesh {
    char key[40];
    render::GpuMesh gpu;
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

private:
    bool load(CachedMesh& entry);

    rhi::Device& device_;
    Allocator& allocator_;
    asset::AssetRegistry& registry_;
    char data_dir_[256];
    Array<CachedMesh> meshes_;
};

} // namespace lsd::editor
