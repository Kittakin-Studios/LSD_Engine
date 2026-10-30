#include "mesh_cache.h"

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

bool make_builtin(const char* name, Allocator& allocator, render::MeshData& out) {
    if (std::strcmp(name, "Cube") == 0) out = render::make_cube(allocator);
    else if (std::strcmp(name, "Sphere") == 0) out = render::make_sphere(allocator);
    else if (std::strcmp(name, "Plane") == 0) out = render::make_plane(allocator);
    else return false;
    return true;
}

} // namespace

MeshCache::MeshCache(rhi::Device& device, Allocator& allocator, asset::AssetRegistry& registry, const char* data_dir)
    : device_(device), allocator_(allocator), registry_(registry), meshes_(allocator) {
    std::snprintf(data_dir_, sizeof data_dir_, "%s", data_dir);
}

MeshCache::~MeshCache() {
    for (CachedMesh& m : meshes_) {
        if (m.gpu.index_count) render::destroy_mesh(device_, m.gpu);
    }
}

bool MeshCache::load(CachedMesh& entry) {
    render::MeshData data;
    asset::MeshBounds bounds;
    asset::Guid guid;
    if (!asset::parse_guid(entry.key, guid)) {
        if (entry.gpu.index_count) return true;  // built-ins never change
        if (!make_builtin(entry.key, allocator_, data)) return false;
        bounds = asset::compute_bounds(data);
    } else {
        const asset::AssetRecord* record = registry_.find(guid);
        if (!record || record->type != asset::AssetType::Mesh) return false;
        char compiled[300];
        if (!asset::compile_mesh_asset(registry_, *record, data_dir_, allocator_, compiled, sizeof compiled)) return false;
        const i64 time = file_time(compiled);
        if (entry.gpu.index_count && time == entry.compiled_time) return true;  // already current
        if (!asset::load_compiled_mesh(compiled, allocator_, data, &bounds)) return false;
        entry.compiled_time = time;
    }
    if (entry.gpu.index_count) render::destroy_mesh(device_, entry.gpu);
    entry.gpu = render::upload_mesh(device_, data);
    entry.bounds_min = {bounds.min[0], bounds.min[1], bounds.min[2]};
    entry.bounds_max = {bounds.max[0], bounds.max[1], bounds.max[2]};
    render::free_mesh(allocator_, data);
    return true;
}

const CachedMesh* MeshCache::get(const char* key) {
    for (const CachedMesh& m : meshes_) {
        if (std::strcmp(m.key, key) == 0) return m.failed ? nullptr : &m;
    }
    CachedMesh entry{};
    std::snprintf(entry.key, sizeof entry.key, "%s", key);
    entry.failed = !load(entry);
    if (entry.failed) LSD_LOG_WARN("editor", "Mesh '%s' is not a basic shape or a mesh asset", key);
    meshes_.push_back(entry);
    return entry.failed ? nullptr : &meshes_[meshes_.size() - 1];
}

void MeshCache::hot_reload() {
    for (CachedMesh& m : meshes_) m.failed = !load(m);
}

} // namespace lsd::editor
