#pragma once
// Objects placed in the editor viewport and the GPU meshes they use.
// shortcut: a flat list until the World (entities + components, milestone 3) replaces it.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/math.h"
#include "lsd/render/mesh.h"

namespace lsd::editor {

struct SceneObject {
    char name[64];
    u32 mesh;          // index into the mesh cache
    Vec3 position;
    f32 color[4];
};

class EditorScene {
public:
    EditorScene(rhi::Device& device, Allocator& allocator, asset::AssetRegistry& registry, const char* data_dir);
    ~EditorScene();
    EditorScene(const EditorScene&) = delete;
    EditorScene& operator=(const EditorScene&) = delete;

    // Built-in shape (cube, sphere...) placed at `position`; takes ownership of `mesh`.
    void add_builtin(const char* name, render::MeshData& mesh, Vec3 position, const f32 color[4]);
    // Compiles the asset if needed, uploads it once, and places an instance. False if it fails to import.
    bool place_asset(asset::Guid guid, Vec3 position);
    void remove(usize index);

    // Recompiles meshes whose source file or import settings changed and swaps the GPU data in place.
    void hot_reload();

    usize count() const { return objects_.size(); }
    const SceneObject& object(usize i) const { return objects_[i]; }
    const render::GpuMesh& mesh(u32 i) const { return meshes_[i].gpu; }

private:
    struct CachedMesh {
        asset::Guid guid;  // invalid for built-in shapes
        render::GpuMesh gpu;
        i64 compiled_time;
    };

    bool load_compiled(asset::Guid guid, CachedMesh& entry);

    rhi::Device& device_;
    Allocator& allocator_;
    asset::AssetRegistry& registry_;
    char data_dir_[256];
    Array<CachedMesh> meshes_;
    Array<SceneObject> objects_;
};

} // namespace lsd::editor
