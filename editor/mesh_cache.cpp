#include "mesh_cache.h"

#include "asset_compiler.h"
#include "lod_cache.h"

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

// Simpler LODs for meshes with enough triangles to gain from them (render::build_lods), as index buffers over its
// vertices; `cache` (may be null): the file they are read from when current, else written to (lod_cache.h).
void build_lods(rhi::Device& device, Allocator& allocator, const render::MeshData& data, CachedMesh& m, bool lock_border = false, const char* cache = nullptr,
                i64 compiled_time = 0) {
    m.lod_count = 1;
    u32* out[CachedMesh::kMaxLods - 1];
    u32* indices = static_cast<u32*>(allocator.allocate(sizeof(u32) * data.index_count * (CachedMesh::kMaxLods - 1), alignof(u32)));
    for (u32 k = 0; k + 1 < CachedMesh::kMaxLods; ++k) out[k] = indices + static_cast<usize>(k) * data.index_count;
    LodSet lods;
    if (!read_lod_cache(cache, compiled_time, data, m.lod_screen_size, out, lods)) {
        lods = LodSet{};
        lods.lod_count = render::build_lods(data, m.lod_screen_size, out, lods.counts, lock_border);
        write_lod_cache(cache, compiled_time, data, m.lod_screen_size, out, lods);
    }
    m.lod_count = lods.lod_count;
    for (u32 k = 1; k < m.lod_count; ++k) {
        m.lod_indices[k] = device.create_buffer(rhi::BufferUsage::Index, out[k - 1], sizeof(u32) * lods.counts[k - 1]);
        m.lod_index_count[k] = lods.counts[k - 1];
    }
    allocator.deallocate(indices);
}

// A mesh with LODs made in the modelling program (Unreal's _LOD1.. nodes, FBX LOD groups): its vertices and
// theirs in one vertex buffer, each LOD's indices pointing into its own part of it, so the draws are the same
// as for generated LODs. False when the compiled mesh has none.
bool upload_authored_lods(rhi::Device& device, Allocator& allocator, const char* compiled, const render::MeshData& lod0, CachedMesh& m) {
    render::MeshData lods[asset::kMaxAuthoredLods];
    u32 n = 0;
    if (!asset::load_compiled_lods(compiled, allocator, lods, n) || n == 0) return false;
    u32 keep = n < CachedMesh::kMaxLods - 1 ? n : CachedMesh::kMaxLods - 1;
    u32 vertex_count = lod0.vertex_count;
    for (u32 l = 0; l < keep; ++l) vertex_count += lods[l].vertex_count;
    render::MeshData all = render::allocate_mesh(allocator, vertex_count, lod0.index_count);
    std::memcpy(all.vertices, lod0.vertices, sizeof(rhi::MeshVertex) * lod0.vertex_count);
    std::memcpy(all.indices, lod0.indices, sizeof(u32) * lod0.index_count);
    u32 base = lod0.vertex_count;
    for (u32 l = 0; l < keep; ++l) {
        std::memcpy(all.vertices + base, lods[l].vertices, sizeof(rhi::MeshVertex) * lods[l].vertex_count);
        for (u32 i = 0; i < lods[l].index_count; ++i) lods[l].indices[i] += base;
        m.lod_indices[l + 1] = device.create_buffer(rhi::BufferUsage::Index, lods[l].indices, sizeof(u32) * lods[l].index_count);
        m.lod_index_count[l + 1] = lods[l].index_count;
        base += lods[l].vertex_count;
    }
    m.gpu = render::upload_mesh(device, all);
    m.lod_count = 1 + keep;
    render::free_mesh(allocator, all);
    for (u32 l = 0; l < n; ++l) render::free_mesh(allocator, lods[l]);
    return true;
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
    }
    char compiled[300] = "";
    if (is_asset) {
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
    const auto lods_started = std::chrono::steady_clock::now();
    if (boxed || !compiled[0] || !upload_authored_lods(device_, allocator_, compiled, data, entry)) {  // the file's own LODs, else generated ones (cached)
        entry.gpu = render::upload_mesh(device_, data);
        build_lods(device_, allocator_, data, entry, false, lod_cache[0] ? lod_cache : nullptr, entry.compiled_time);
    }
    entry.vertex_bytes = static_cast<u64>(data.vertex_count) * sizeof(rhi::MeshVertex);
    entry.index_bytes = static_cast<u64>(data.index_count) * sizeof(u32);
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

const MeshCache::Hulls* MeshCache::hulls(const char* key) {
    if (const auto it = hulls_.find(key); it != hulls_.end()) return it->second.sizes.empty() ? nullptr : &it->second;
    Hulls& h = hulls_[key];
    asset::Guid guid;
    i32 part = -1;
    const asset::AssetRecord* record = asset::parse_mesh_key(key, guid, part) && part < 0 ? registry_.find(guid) : nullptr;
    if (!record || record->type != asset::AssetType::Mesh) return nullptr;
    asset::AssetMeta meta;
    registry_.load_meta(*record, meta);
    char source[300], error[256];
    registry_.source_path(*record, source, sizeof source);
    Array<Vec3> points(allocator_);
    Array<u32> sizes(allocator_);
    if (!asset::import_collision_hulls(source, meta.import_scale, allocator_, points, sizes, error, sizeof error)) {
        LSD_LOG_WARN("mesh", "%s: collision nodes not read: %s", record->path, error);
        return nullptr;
    }
    h.points.assign(points.data(), points.data() + points.size());
    h.sizes.assign(sizes.data(), sizes.data() + sizes.size());
    if (h.sizes.empty()) LSD_LOG_WARN("mesh", "%s: Collision Shape is Collision Nodes, but the file has no UCX_ / UBX_ / USP_ / UCP_ nodes: its Box is used", record->path);
    else LSD_LOG_INFO("mesh", "%s: %zu collision hulls from the file", record->path, h.sizes.size());
    return h.sizes.empty() ? nullptr : &h;
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

namespace {

// The mesh key without a "|box:" suffix (what the compiler and the registry know).
void plain_key(const char* key, char (&out)[80]) {
    std::snprintf(out, sizeof out, "%s", key);
    if (char* suffix = std::strstr(out, "|box:")) *suffix = '\0';
}

i64 now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

} // namespace

bool MeshCache::finish(u32 slot) {
    CachedMesh& m = meshes_[slot];
    char plain[80];
    plain_key(m.key, plain);
    if (compiler::mesh_busy(plain, data_dir_)) return false;
    // Loading a big mesh (read, upload, LODs) takes milliseconds: at most ~4 ms of them per 16 ms, so a burst of
    // finished compiles does not stall a frame.
    const i64 now = now_ns();
    if (now - finish_window_ > 16000000) finish_window_ = now, finish_spent_ = 0;
    if (finish_spent_ > 4000000) return false;
    pending_.erase(slot);
    m.failed = !load(m);
    finish_spent_ += now_ns() - now;
    stamps_[slot] = m.failed ? 0 : source_stamp(m.key);
    if (m.failed) LSD_LOG_WARN("editor", "Mesh '%s' is not a basic shape or a mesh asset", m.key);
    return true;
}

void MeshCache::finish_pending() {
    if (pending_.empty()) return;
    compiler::wait();
    finish_spent_ = 0;
    const std::vector<u32> slots(pending_.begin(), pending_.end());
    for (const u32 slot : slots) {
        finish_window_ = now_ns();  // no frame to spread over: all of them now
        finish_spent_ = 0;
        finish(slot);
    }
}

const CachedMesh* MeshCache::get(const char* key) {
    const i64 slot = index_.find(key, 0, [&](u32 i) { return std::strcmp(meshes_[i].key, key) == 0; });
    if (slot >= 0) {
        if (meshes_[static_cast<usize>(slot)].failed && pending_.count(static_cast<u32>(slot))) finish(static_cast<u32>(slot));
        return meshes_[static_cast<usize>(slot)].failed ? nullptr : &meshes_[static_cast<usize>(slot)];
    }
    if (std::strncmp(key, "Landscape:", 10) == 0 || std::strncmp(key, "EditMesh:", 9) == 0 || std::strncmp(key, "Sprite:", 7) == 0 || std::strncmp(key, "TileMap:", 8) == 0 || std::strncmp(key, "UiPanel:", 8) == 0 || std::strncmp(key, "Lightmap:", 9) == 0 || std::strncmp(key, "HLOD:", 5) == 0 || std::strncmp(key, "Cloth:", 6) == 0) return nullptr;  // not generated yet
    CachedMesh entry{};
    entry.lod_count = 1;
    std::snprintf(entry.key, sizeof entry.key, "%s", key);
    char plain[80];
    plain_key(key, plain);
    const bool later = compiler::queue_mesh(registry_, plain, data_dir_);  // not compiled yet: on a worker thread, drawn when done
    if (later) pending_.insert(static_cast<u32>(meshes_.size()));
    entry.failed = later || !load(entry);
    if (entry.failed && !later) LSD_LOG_WARN("editor", "Mesh '%s' is not a basic shape or a mesh asset", key);
    index_.add(key, 0, static_cast<u32>(meshes_.size()));
    meshes_.push_back(entry);
    stamps_.resize(meshes_.size(), 0);
    stamps_.back() = entry.failed ? 0 : source_stamp(key);
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

i64 MeshCache::source_stamp(const char* key) const {
    char plain[80];  // without a "|box:" suffix
    std::snprintf(plain, sizeof plain, "%s", key);
    if (char* suffix = std::strstr(plain, "|box:")) *suffix = '\0';
    asset::Guid guid;
    i32 part = -1;
    const asset::AssetRecord* record = asset::parse_mesh_key(plain, guid, part) ? registry_.find(guid) : nullptr;
    return record ? static_cast<i64>(static_cast<u64>(record->source_time) * 31u + static_cast<u64>(record->meta_time) + 1u) : 0;
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
    // Only meshes whose file or .meta changed are looked at again: re-reading every cached mesh's .meta and compiled
    // header once a second was a 50-100 ms hitch each second with a few hundred meshes.
    stamps_.resize(meshes_.size(), 0);
    if (registry_.generation() != slot_parts_generation_) slot_parts_.clear();  // a file or .meta changed: Material Slots or parts may have
    slot_parts_generation_ = registry_.generation();
    for (usize i = 0; i < meshes_.size(); ++i) {
        CachedMesh& m = meshes_[i];
        const i64 stamp = m.generated ? 0 : source_stamp(m.key);
        if (m.generated || (stamp && stamp == stamps_[i] && !m.failed) || pending_.count(static_cast<u32>(i))) continue;
        const i64 before = m.compiled_time;
        m.failed = !load(m);
        if (m.compiled_time != before || m.failed) {  // reimported: its triangles and hulls are re-read on the next ask
            triangles_.erase(m.key);
            hulls_.erase(m.key);
        }
        stamps_[i] = m.failed ? 0 : stamp;
    }
}

} // namespace lsd::editor
