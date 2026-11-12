#include "mesh_cache.h"

#include "lsd/asset/asset_meta.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"

#include <chrono>

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
    // shortcut: room for 8192 meshes (shapes, assets, LOD / box-UV variants, sprites, tiles) without moving; grow by chunks if a project needs more.
    meshes_.reserve(8192);
}

namespace {

void destroy_lods(rhi::Device& device, CachedMesh& m) {
    for (u32 i = 1; i < m.lod_count; ++i) device.destroy_buffer(m.lod_indices[i]);
    m.lod_count = 1;
}

} // namespace

u32 CachedMesh::pick_lod(f32 radius_over_distance, i32 force_lod) const { return render::pick_lod(lod_screen_size, lod_count, radius_over_distance, force_lod); }

namespace {

// The LODs' indices kept in DerivedData beside the compiled mesh ("<compiled>.lod"), so a mesh loaded again reads them
// instead of simplifying it again (most of a dense mesh's load time). Stale when the compiled mesh, its size or the
// LOD screen sizes changed.
struct LodFileHeader {
    char magic[4];
    u32 version;
    i64 compiled_time;
    u32 index_count, lod_count;
    f32 sizes[CachedMesh::kMaxLods];
    u32 counts[CachedMesh::kMaxLods - 1];
};
constexpr char kLodMagic[4] = {'L', 'S', 'D', 'L'};
constexpr u32 kLodVersion = 1;  // bump when render::build_lods changes what it makes

// Simpler LODs for meshes with enough triangles to gain from them (render::build_lods), as index buffers over its
// vertices; `cache` (may be null): the file they are read from when current, else written to.
void build_lods(rhi::Device& device, Allocator& allocator, const render::MeshData& data, CachedMesh& m, bool lock_border = false, const char* cache = nullptr,
                i64 compiled_time = 0) {
    m.lod_count = 1;
    u32* out[CachedMesh::kMaxLods - 1];
    u32* indices = static_cast<u32*>(allocator.allocate(sizeof(u32) * data.index_count * (CachedMesh::kMaxLods - 1), alignof(u32)));
    for (u32 k = 0; k + 1 < CachedMesh::kMaxLods; ++k) out[k] = indices + static_cast<usize>(k) * data.index_count;
    LodFileHeader h{};
    bool cached = false;
    if (FILE* f = cache ? std::fopen(cache, "rb") : nullptr) {
        cached = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kLodMagic, 4) == 0 && h.version == kLodVersion && h.compiled_time == compiled_time &&
                 h.index_count == data.index_count && h.lod_count >= 1 && h.lod_count <= CachedMesh::kMaxLods &&
                 std::memcmp(h.sizes, m.lod_screen_size, sizeof h.sizes) == 0;
        for (u32 k = 1; cached && k < h.lod_count; ++k) {
            cached = h.counts[k - 1] <= data.index_count && std::fread(out[k - 1], sizeof(u32), h.counts[k - 1], f) == h.counts[k - 1];
            for (u32 i = 0; cached && i < h.counts[k - 1]; ++i) cached = out[k - 1][i] < data.vertex_count;  // a damaged file must not index past the vertices
        }
        std::fclose(f);
    }
    if (!cached) {
        h = LodFileHeader{};
        std::memcpy(h.magic, kLodMagic, 4);
        h.version = kLodVersion;
        h.compiled_time = compiled_time;
        h.index_count = data.index_count;
        std::memcpy(h.sizes, m.lod_screen_size, sizeof h.sizes);
        h.lod_count = render::build_lods(data, m.lod_screen_size, out, h.counts, lock_border);
        if (FILE* f = cache ? std::fopen(cache, "wb") : nullptr) {
            bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
            for (u32 k = 1; ok && k < h.lod_count; ++k) ok = std::fwrite(out[k - 1], sizeof(u32), h.counts[k - 1], f) == h.counts[k - 1];
            std::fclose(f);
            if (!ok) std::remove(cache);  // a disk full: built again next time
        }
    }
    m.lod_count = h.lod_count;
    for (u32 k = 1; k < m.lod_count; ++k) {
        m.lod_indices[k] = device.create_buffer(rhi::BufferUsage::Index, out[k - 1], sizeof(u32) * h.counts[k - 1]);
        m.lod_index_count[k] = h.counts[k - 1];
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

namespace {

f64 ms_since(std::chrono::steady_clock::time_point start) { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count(); }

} // namespace

bool MeshCache::load(CachedMesh& entry) {
    if (entry.generated) return true;  // nothing on disk to reload
    const auto started = std::chrono::steady_clock::now();
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
    render::lod_screen_sizes(meta.lod1_screen_size, meta.lod2_screen_size, meta.lod3_screen_size, entry.lod_screen_size);
    char lod_cache[320] = "";
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
        std::snprintf(lod_cache, sizeof lod_cache, "%s.lod", compiled);  // Box UV variants share it: same positions
    }
    if (boxed) render::box_project_uv(data, box.x, box.y, box.z);
    if (entry.gpu.index_count) render::destroy_mesh(device_, entry.gpu);
    destroy_lods(device_, entry);
    entry.gpu = render::upload_mesh(device_, data);
    entry.vertex_bytes = static_cast<u64>(data.vertex_count) * sizeof(rhi::MeshVertex);
    entry.index_bytes = static_cast<u64>(data.index_count) * sizeof(u32);
    const auto lods_started = std::chrono::steady_clock::now();
    build_lods(device_, allocator_, data, entry, false, lod_cache[0] ? lod_cache : nullptr, entry.compiled_time);
    lod_ms_ += ms_since(lods_started);
    entry.bounds_min = {bounds.min[0], bounds.min[1], bounds.min[2]};
    entry.bounds_max = {bounds.max[0], bounds.max[1], bounds.max[2]};
    render::free_mesh(allocator_, data);
    load_ms_ += ms_since(started);
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
    for (u32 i = 0; i < data.vertex_count; ++i) t.uvs.insert(t.uvs.end(), data.vertices[i].uv, data.vertices[i].uv + 2);
    t.indices.assign(data.indices, data.indices + data.index_count);
    render::free_mesh(allocator_, data);
    return &t;
}

const std::vector<MeshCache::SlotPart>* MeshCache::slot_parts(const char* key, bool all) {
    const std::string cache_key = all ? std::string(key) + "|all" : std::string(key);
    if (const auto it = slot_parts_.find(cache_key); it != slot_parts_.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<SlotPart>& out = slot_parts_[cache_key];
    asset::Guid guid;
    i32 part = -1;
    const asset::AssetRecord* record = asset::parse_mesh_key(key, guid, part) && part < 0 ? registry_.find(guid) : nullptr;
    if (!record || record->type != asset::AssetType::Mesh) return nullptr;
    asset::AssetMeta meta;
    registry_.load_meta(*record, meta);
    bool any = false;
    for (const asset::MaterialSlot& s : meta.material_slots) any = any || s.material[0];
    if (!any && !all) return nullptr;
    char source[300], error[256];
    registry_.source_path(*record, source, sizeof source);
    Array<asset::MeshPart> parts(allocator_);
    if (!asset::list_mesh_parts(source, parts, error, sizeof error) || parts.empty()) return nullptr;
    for (usize k = 0; k < parts.size(); ++k) {
        SlotPart p{};
        if (parts.size() == 1) {  // one part: the whole mesh as it is, with that slot's Material
            std::snprintf(p.key, sizeof p.key, "%s", key);
        } else {
            std::snprintf(p.key, sizeof p.key, "%s#%zu", key, k);
            p.local = trs(parts[k].position * meta.import_scale, parts[k].rotation, parts[k].scale);  // its mesh is scaled by import_scale too
        }
        std::snprintf(p.slot, sizeof p.slot, "%s", parts[k].material);
        for (const asset::MaterialSlot& s : meta.material_slots) {
            if (parts[k].material[0] && std::strcmp(s.name, parts[k].material) == 0) std::snprintf(p.material, sizeof p.material, "%s", s.material);
        }
        out.push_back(p);
    }
    return &out;
}

const CachedMesh* MeshCache::get(const char* key) {
    const i64 slot = index_.find(key, 0, [&](u32 i) { return std::strcmp(meshes_[i].key, key) == 0; });
    if (slot >= 0) return meshes_[static_cast<usize>(slot)].failed ? nullptr : &meshes_[static_cast<usize>(slot)];
    if (std::strncmp(key, "Landscape:", 10) == 0 || std::strncmp(key, "EditMesh:", 9) == 0 || std::strncmp(key, "Sprite:", 7) == 0 || std::strncmp(key, "TileMap:", 8) == 0 || std::strncmp(key, "UiPanel:", 8) == 0 || std::strncmp(key, "Lightmap:", 9) == 0 || std::strncmp(key, "HLOD:", 5) == 0 || std::strncmp(key, "Cloth:", 6) == 0) return nullptr;  // not generated yet
    CachedMesh entry{};
    entry.lod_count = 1;
    std::snprintf(entry.key, sizeof entry.key, "%s", key);
    entry.failed = !load(entry);
    if (entry.failed) LSD_LOG_WARN("editor", "Mesh '%s' is not a basic shape or a mesh asset", key);
    index_.add(key, 0, static_cast<u32>(meshes_.size()));
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

void MeshCache::set_generated(const char* key, const render::MeshData& data) {
    CachedMesh* entry = nullptr;
    const i64 slot = index_.find(key, 0, [&](u32 i) { return std::strcmp(meshes_[i].key, key) == 0; });
    if (slot >= 0) entry = &meshes_[static_cast<usize>(slot)];
    if (!entry) {
        CachedMesh fresh{};
        std::snprintf(fresh.key, sizeof fresh.key, "%s", key);
        index_.add(key, 0, static_cast<u32>(meshes_.size()));
        meshes_.push_back(fresh);
        entry = &meshes_[meshes_.size() - 1];
    }
    if (entry->gpu.index_count) render::destroy_mesh(device_, entry->gpu);
    destroy_lods(device_, *entry);
    entry->gpu = render::upload_mesh(device_, data);
    entry->vertex_bytes = static_cast<u64>(data.vertex_count) * sizeof(rhi::MeshVertex);
    entry->index_bytes = static_cast<u64>(data.index_count) * sizeof(u32);
    entry->lod_count = 1;
    entry->lod_screen_size[0] = 1e30f;
    if (std::strncmp(key, "Landscape:", 10) == 0) {  // terrain LODs (Unreal's landscape LODs); edges locked so neighbouring tiles meet
        const asset::AssetMeta defaults;
        render::lod_screen_sizes(defaults.lod1_screen_size, defaults.lod2_screen_size, defaults.lod3_screen_size, entry->lod_screen_size);
        build_lods(device_, allocator_, data, *entry, true);
    }
    const asset::MeshBounds bounds = asset::compute_bounds(data);
    entry->bounds_min = {bounds.min[0], bounds.min[1], bounds.min[2]};
    entry->bounds_max = {bounds.max[0], bounds.max[1], bounds.max[2]};
    entry->generated = true;
    entry->failed = false;
    Triangles& t = triangles_[key];
    t.points.clear();
    t.uvs.clear();
    for (u32 i = 0; i < data.vertex_count; ++i) t.points.push_back({data.vertices[i].position[0], data.vertices[i].position[1], data.vertices[i].position[2]});
    for (u32 i = 0; i < data.vertex_count; ++i) t.uvs.insert(t.uvs.end(), data.vertices[i].uv, data.vertices[i].uv + 2);
    t.indices.assign(data.indices, data.indices + data.index_count);
}

MeshCache::Memory MeshCache::memory() const {
    Memory out;
    out.load_ms = load_ms_;
    out.lod_ms = lod_ms_;
    for (const CachedMesh& m : meshes_) {
        if (!m.gpu.index_count) continue;
        ++out.meshes;
        out.vertex_bytes += m.vertex_bytes;
        out.index_bytes += m.index_bytes;
        for (u32 k = 1; k < m.lod_count; ++k) out.lod_index_bytes += static_cast<u64>(m.lod_index_count[k]) * sizeof(u32);
    }
    return out;
}

void MeshCache::hot_reload() {
    slot_parts_.clear();  // Material Slots or parts may have changed
    for (CachedMesh& m : meshes_) {
        const i64 before = m.compiled_time;
        m.failed = !load(m);
        if (m.compiled_time != before || m.failed) triangles_.erase(m.key);  // reimported: its triangles are re-read on the next ask
    }
}

} // namespace lsd::editor
