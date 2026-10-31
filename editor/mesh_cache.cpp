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

namespace {

void destroy_lods(rhi::Device& device, CachedMesh& m) {
    for (u32 i = 1; i < m.lod_count; ++i) device.destroy_buffer(m.lod_indices[i]);
    m.lod_count = 1;
}

} // namespace

u32 CachedMesh::pick_lod(f32 radius_over_distance, i32 force_lod) const {
    if (force_lod >= 0) return static_cast<u32>(force_lod) < lod_count ? static_cast<u32>(force_lod) : lod_count - 1;
    // shortcut: fixed angular sizes (about half, a quarter, an eighth of the screen at 60 degrees), add per-mesh screen sizes when a project needs them
    const f32 kSizes[] = {0.12f, 0.06f, 0.03f};
    u32 lod = 0;
    while (lod + 1 < lod_count && radius_over_distance < kSizes[lod]) ++lod;
    return lod;
}

namespace {

// Simpler LODs for meshes with enough triangles to gain from them.
void build_lods(rhi::Device& device, Allocator& allocator, const render::MeshData& data, CachedMesh& m) {
    m.lod_count = 1;
    constexpr u32 kMinTriangles = 512;
    if (data.index_count / 3 < kMinTriangles) return;
    u32* indices = static_cast<u32*>(allocator.allocate(sizeof(u32) * data.index_count, alignof(u32)));
    u32 previous = data.index_count;
    f32 ratio = 0.5f;
    for (u32 lod = 1; lod < CachedMesh::kMaxLods; ++lod, ratio *= 0.5f) {
        const u32 count = asset::simplify_mesh(data, ratio, indices);
        if (count == 0 || count > previous * 3 / 4) break;  // the shape will not get simpler: stop
        m.lod_indices[lod] = device.create_buffer(rhi::BufferUsage::Index, indices, sizeof(u32) * count);
        m.lod_index_count[lod] = count;
        m.lod_count = lod + 1;
        previous = count;
    }
    allocator.deallocate(indices);
}

} // namespace

MeshCache::~MeshCache() {
    for (CachedMesh& m : meshes_) {
        if (m.gpu.index_count) render::destroy_mesh(device_, m.gpu);
        destroy_lods(device_, m);
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
    destroy_lods(device_, entry);
    entry.gpu = render::upload_mesh(device_, data);
    build_lods(device_, allocator_, data, entry);
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
    entry.lod_count = 1;
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
