#include "mesh_cache.h"

#include "lsd/asset/asset_meta.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"

#include <cmath>
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
    u32 lod = 0;
    while (lod + 1 < lod_count && radius_over_distance < lod_screen_size[lod + 1]) ++lod;
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
    i32 part = -1;
    char key[80];  // without a "|box:" suffix
    std::snprintf(key, sizeof key, "%s", entry.key);
    Vec3 box;
    char* suffix = std::strstr(key, "|box:");
    const bool boxed = suffix && std::sscanf(suffix + 5, "%f,%f,%f", &box.x, &box.y, &box.z) == 3;
    if (suffix) *suffix = '\0';
    const bool is_asset = asset::parse_mesh_key(key, guid, part);
    asset::AssetMeta meta;  // LOD screen sizes, re-read on every reload (editing them does not recompile; defaults for shapes)
    if (const asset::AssetRecord* record = is_asset ? registry_.find(guid) : nullptr) registry_.load_meta(*record, meta);
    entry.lod_screen_size[0] = 1e30f;
    entry.lod_screen_size[1] = meta.lod1_screen_size;
    entry.lod_screen_size[2] = meta.lod2_screen_size;
    entry.lod_screen_size[3] = meta.lod3_screen_size;
    if (!is_asset) {
        if (entry.gpu.index_count) return true;  // built-ins never change
        if (!make_builtin(key, allocator_, data)) return false;
        bounds = asset::compute_bounds(data);
    } else {
        char compiled[300];
        if (!asset::compile_mesh_key(registry_, key, data_dir_, allocator_, compiled, sizeof compiled)) return false;
        const i64 time = file_time(compiled);
        if (entry.gpu.index_count && time == entry.compiled_time) return true;  // already current
        if (!asset::load_compiled_mesh(compiled, allocator_, data, &bounds)) return false;
        entry.compiled_time = time;
    }
    if (boxed) {  // project each vertex onto the plane its normal faces most, in metres
        for (u32 i = 0; i < data.vertex_count; ++i) {
            rhi::MeshVertex& v = data.vertices[i];
            const f32 x = v.position[0] * box.x, y = v.position[1] * box.y, z = v.position[2] * box.z;
            const f32 ax = std::fabs(v.normal[0]), ay = std::fabs(v.normal[1]), az = std::fabs(v.normal[2]);
            const bool side_x = ax >= ay && ax >= az, side_y = !side_x && ay >= az;
            v.uv[0] = side_x ? (v.normal[0] > 0.0f ? y : -y) : side_y ? (v.normal[1] > 0.0f ? -x : x) : x;
            v.uv[1] = side_x || side_y ? -z : (v.normal[2] > 0.0f ? -y : y);  // walls upright
        }
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

const MeshCache::Triangles* MeshCache::triangles(const char* key) {
    if (const auto it = triangles_.find(key); it != triangles_.end()) return &it->second;
    render::MeshData data;
    asset::Guid guid;
    i32 part = -1;
    if (!asset::parse_mesh_key(key, guid, part)) {
        if (!make_builtin(key, allocator_, data)) return nullptr;
    } else {
        char compiled[300];
        if (!asset::compile_mesh_key(registry_, key, data_dir_, allocator_, compiled, sizeof compiled) || !asset::load_compiled_mesh(compiled, allocator_, data, nullptr)) {
            return nullptr;
        }
    }
    Triangles& t = triangles_[key];
    t.points.reserve(data.vertex_count);
    for (u32 i = 0; i < data.vertex_count; ++i) t.points.push_back({data.vertices[i].position[0], data.vertices[i].position[1], data.vertices[i].position[2]});
    t.indices.assign(data.indices, data.indices + data.index_count);
    render::free_mesh(allocator_, data);
    return &t;
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

const CachedMesh* MeshCache::get_box_uv(const char* mesh, Vec3 scale) {
    auto cm = [](f32 v) { return std::round(std::fabs(v) * 100.0f) / 100.0f; };
    char key[80];
    std::snprintf(key, sizeof key, "%s|box:%g,%g,%g", mesh, static_cast<f64>(cm(scale.x)), static_cast<f64>(cm(scale.y)), static_cast<f64>(cm(scale.z)));
    const CachedMesh* boxed = get(key);
    return boxed ? boxed : get(mesh);
}

void MeshCache::hot_reload() {
    for (CachedMesh& m : meshes_) {
        const i64 before = m.compiled_time;
        m.failed = !load(m);
        if (m.compiled_time != before || m.failed) triangles_.erase(m.key);  // reimported: its triangles are re-read on the next ask
    }
}

} // namespace lsd::editor
