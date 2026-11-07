#include "paper2d.h"

#include "mesh_cache.h"

#include "lsd/asset/asset_registry.h"
#include "lsd/asset/data_asset.h"
#include "lsd/asset/guid.h"
#include "lsd/render/mesh.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

namespace {

constexpr f32 kInset = 1e-4f;  // UVs stay just inside their cell: whole-texel sampling never reads the neighbour

// One quad facing -Y and its back (facing +Y), x0..x1 by z0..z1 at y = 0, with a cell's UVs.
void add_quad(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 x0, f32 z0, f32 x1, f32 z1, f32 u0, f32 v0, f32 u1, f32 v1) {
    for (int side = 0; side < 2; ++side) {
        const u32 base = static_cast<u32>(v.size());
        const f32 ny = side == 0 ? -1.0f : 1.0f;
        v.push_back({{x0, 0, z0}, {0, ny, 0}, {u0, v1}});
        v.push_back({{x1, 0, z0}, {0, ny, 0}, {u1, v1}});
        v.push_back({{x1, 0, z1}, {0, ny, 0}, {u1, v0}});
        v.push_back({{x0, 0, z1}, {0, ny, 0}, {u0, v0}});
        const u32 front[6] = {0, 1, 2, 0, 2, 3}, back[6] = {0, 2, 1, 0, 3, 2};
        for (u32 i : side == 0 ? front : back) idx.push_back(base + i);
    }
}

// The UV rectangle of cell `cell` in a columns x rows sheet (row 0 at the top of the image).
void cell_uv(u32 cell, u32 columns, u32 rows, f32& u0, f32& v0, f32& u1, f32& v1) {
    columns = columns ? columns : 1;
    rows = rows ? rows : 1;
    const u32 col = cell % columns, row = (cell / columns) % rows;
    u0 = static_cast<f32>(col) / static_cast<f32>(columns) + kInset;
    u1 = static_cast<f32>(col + 1) / static_cast<f32>(columns) - kInset;
    v0 = static_cast<f32>(row) / static_cast<f32>(rows) + kInset;
    v1 = static_cast<f32>(row + 1) / static_cast<f32>(rows) - kInset;
}

const CachedMesh* upload(MeshCache& meshes, const char* key, const std::vector<rhi::MeshVertex>& v, const std::vector<u32>& idx) {
    render::MeshData data = render::allocate_mesh(default_allocator(), static_cast<u32>(v.size()), static_cast<u32>(idx.size()));
    for (usize i = 0; i < v.size(); ++i) data.vertices[i] = v[i];
    for (usize i = 0; i < idx.size(); ++i) data.indices[i] = idx[i];
    meshes.set_generated(key, data);
    render::free_mesh(default_allocator(), data);
    return meshes.get(key);
}

} // namespace

u32 sprite_cell(const world::Sprite& sprite, f32 seconds, const world::Flipbook* flipbook) {
    if (flipbook) {
        const i32 frame = flipbook_frame(*flipbook, seconds);
        if (frame >= 0) return flipbook->frames[static_cast<u32>(frame)].cell;
    }
    const u32 count = sprite.frame_count ? sprite.frame_count : 1;
    if (count == 1 || sprite.fps <= 0.0f) return sprite.frame;
    const u32 step = static_cast<u32>(std::floor(seconds * sprite.fps));
    // shortcut: a one-shot flipbook counts from the clock's start, not from when it was set; add a start time when scripts need replays.
    return sprite.frame + (sprite.loop ? step % count : (step < count ? step : count - 1));
}

const CachedMesh* sprite_mesh(MeshCache& meshes, const world::Sprite& sprite, u32 cell) {
    char key[64];
    std::snprintf(key, sizeof key, "Sprite:%ux%u:%u:%d%d", sprite.columns, sprite.rows, cell, sprite.flip_x ? 1 : 0, sprite.flip_y ? 1 : 0);
    if (const CachedMesh* mesh = meshes.get(key)) return mesh;
    f32 u0, v0, u1, v1;
    cell_uv(cell, sprite.columns, sprite.rows, u0, v0, u1, v1);
    if (sprite.flip_x) std::swap(u0, u1);
    if (sprite.flip_y) std::swap(v0, v1);
    std::vector<rhi::MeshVertex> v;
    std::vector<u32> idx;
    add_quad(v, idx, -0.5f, 0.0f, 0.5f, 1.0f, u0, v0, u1, v1);
    return upload(meshes, key, v, idx);
}

const CachedMesh* tile_map_mesh(MeshCache& meshes, const world::TileMap& map, u32 id) {
    u32 hash = 2166136261u;  // FNV-1a over everything the mesh depends on
    auto mix = [&hash](u32 x) {
        for (int b = 0; b < 4; ++b) hash = (hash ^ ((x >> (b * 8)) & 0xFFu)) * 16777619u;
    };
    mix(map.width);
    mix(map.height);
    mix(map.tileset_columns);
    mix(map.tileset_rows);
    mix(static_cast<u32>(map.tile_size * 1000.0f));
    for (u32 i = 0; i < map.tiles.count; ++i) mix(static_cast<u32>(map.tiles[i]));
    // One mesh per map (keyed by its object), rebuilt in place when the hash of its tiles changes.
    static std::unordered_map<u32, u32> built;
    char key[32];
    std::snprintf(key, sizeof key, "TileMap:%u", id);
    const auto at = built.find(id);
    if (at != built.end() && at->second == hash) {
        if (const CachedMesh* mesh = meshes.get(key)) return mesh;
    }
    built[id] = hash;
    std::vector<rhi::MeshVertex> v;
    std::vector<u32> idx;
    const f32 s = map.tile_size;
    for (u32 y = 0; y < map.height; ++y) {
        for (u32 x = 0; x < map.width; ++x) {
            const u32 i = y * map.width + x;
            const i32 tile = i < map.tiles.count ? map.tiles[i] : 0;
            if (tile <= 0) continue;
            f32 u0, v0, u1, v1;
            cell_uv(static_cast<u32>(tile - 1), map.tileset_columns, map.tileset_rows, u0, v0, u1, v1);
            add_quad(v, idx, static_cast<f32>(x) * s, static_cast<f32>(y) * s, static_cast<f32>(x + 1) * s, static_cast<f32>(y + 1) * s, u0, v0, u1, v1);
        }
    }
    if (idx.empty()) add_quad(v, idx, 0, 0, 0, 0, 0, 0, 0, 0);  // nothing painted yet: an empty quad keeps the key
    return upload(meshes, key, v, idx);
}

void sprite_bounds(Vec3& lo, Vec3& hi) {
    lo = {-0.5f, -0.05f, 0.0f};
    hi = {0.5f, 0.05f, 1.0f};
}

void tile_map_bounds(const world::TileMap& map, Vec3& lo, Vec3& hi) {
    lo = {0.0f, -0.05f, 0.0f};
    hi = {static_cast<f32>(map.width) * map.tile_size, 0.05f, static_cast<f32>(map.height) * map.tile_size};
}

// shortcut: a file-time check per asset at most once a second, on the main thread; hook the content watcher if flipbooks get many
const world::Flipbook* find_flipbook(const asset::AssetRegistry* registry, const char* guid) {
    if (!registry || !guid || !guid[0]) return nullptr;
    struct Entry {
        world::Flipbook flipbook;
        std::filesystem::file_time_type time;
        u64 checked = 0;  // milliseconds (steady clock)
        bool loaded = false;
    };
    static std::unordered_map<std::string, Entry> cache;
    Entry& e = cache[guid];
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (e.checked && now - e.checked < 1000) return e.loaded ? &e.flipbook : nullptr;
    e.checked = now;
    asset::Guid id;
    const asset::AssetRecord* r = asset::parse_guid(guid, id) ? registry->find(id) : nullptr;
    if (!r) {
        e.loaded = false;
        return nullptr;
    }
    char path[300];
    registry->source_path(*r, path, sizeof path);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (!e.loaded || time != e.time) {
        world::Flipbook fresh;
        e.loaded = asset::load_data_asset(path, fresh);
        if (e.loaded) e.flipbook = fresh;
        e.time = time;
    }
    return e.loaded ? &e.flipbook : nullptr;
}

} // namespace lsd::editor
