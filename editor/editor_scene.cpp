#include "editor_scene.h"

#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::editor {

namespace {

i64 file_time(const char* path) {
    std::error_code error;
    return static_cast<i64>(std::filesystem::last_write_time(path, error).time_since_epoch().count());
}

} // namespace

EditorScene::EditorScene(rhi::Device& device, Allocator& allocator, asset::AssetRegistry& registry, const char* data_dir)
    : device_(device), allocator_(allocator), registry_(registry), meshes_(allocator), objects_(allocator) {
    std::snprintf(data_dir_, sizeof data_dir_, "%s", data_dir);
}

EditorScene::~EditorScene() {
    for (CachedMesh& m : meshes_) render::destroy_mesh(device_, m.gpu);
}

void EditorScene::add_builtin(const char* name, render::MeshData& mesh, Vec3 position, const f32 color[4]) {
    meshes_.push_back(CachedMesh{{}, render::upload_mesh(device_, mesh), 0});
    render::free_mesh(allocator_, mesh);
    SceneObject o{};
    std::snprintf(o.name, sizeof o.name, "%s", name);
    o.mesh = static_cast<u32>(meshes_.size() - 1);
    o.position = position;
    std::memcpy(o.color, color, sizeof o.color);
    objects_.push_back(o);
}

bool EditorScene::load_compiled(asset::Guid guid, CachedMesh& entry) {
    const asset::AssetRecord* record = registry_.find(guid);
    if (!record) return false;
    char compiled[300];
    if (!asset::compile_mesh_asset(registry_, *record, data_dir_, allocator_, compiled, sizeof compiled)) return false;
    const i64 time = file_time(compiled);
    if (entry.gpu.index_count && time == entry.compiled_time) return true;  // already current
    render::MeshData data;
    if (!asset::load_compiled_mesh(compiled, allocator_, data)) return false;
    if (entry.gpu.index_count) render::destroy_mesh(device_, entry.gpu);
    entry.gpu = render::upload_mesh(device_, data);
    entry.compiled_time = time;
    render::free_mesh(allocator_, data);
    return true;
}

bool EditorScene::place_asset(asset::Guid guid, Vec3 position) {
    const asset::AssetRecord* record = registry_.find(guid);
    if (!record || record->type != asset::AssetType::Mesh) return false;

    u32 mesh = static_cast<u32>(meshes_.size());
    for (u32 i = 0; i < meshes_.size(); ++i) {
        if (meshes_[i].guid == guid) mesh = i;
    }
    if (mesh == meshes_.size()) {
        CachedMesh entry{guid, {}, 0};
        if (!load_compiled(guid, entry)) return false;
        meshes_.push_back(entry);
    }

    // Unreal-style names: Chair, Chair2, Chair3...
    SceneObject o{};
    int copies = 0;
    for (const SceneObject& other : objects_) {
        if (other.mesh == mesh) ++copies;
    }
    if (copies) std::snprintf(o.name, sizeof o.name, "%s%d", record->name, copies + 1);
    else std::snprintf(o.name, sizeof o.name, "%s", record->name);
    o.mesh = mesh;
    o.position = position;
    const f32 color[4] = {0.75f, 0.75f, 0.75f, 1.0f};
    std::memcpy(o.color, color, sizeof color);
    objects_.push_back(o);
    LSD_LOG_INFO("editor", "Placed %s at (%.2f, %.2f, %.2f)", o.name, static_cast<f64>(position.x), static_cast<f64>(position.y),
                 static_cast<f64>(position.z));
    return true;
}

void EditorScene::remove(usize index) {
    if (index >= objects_.size()) return;
    // Keep Outliner order stable: shift down instead of swapping.
    for (usize i = index; i + 1 < objects_.size(); ++i) objects_[i] = objects_[i + 1];
    objects_.resize(objects_.size() - 1);
}

void EditorScene::hot_reload() {
    for (CachedMesh& m : meshes_) {
        if (m.guid.valid() && registry_.find(m.guid)) load_compiled(m.guid, m);
    }
}

} // namespace lsd::editor
