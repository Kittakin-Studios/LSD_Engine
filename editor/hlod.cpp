#include "hlod.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"
#include "lsd/render/material.h"
#include "lsd/render/mesh.h"
#include "lsd/world/components.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

namespace lsd::editor {

bool load_level_mesh(const char* key, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator, render::MeshData& out);  // light_bake.cpp

void HlodData::index() {
    cell_of.clear();
    for (usize c = 0; c < cells.size(); ++c) {
        for (const u64 k : cells[c].members) cell_of[k] = static_cast<u32>(c);
    }
}

u64 hlod_member_key(const char* name, const char* mesh, Vec3 position) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* p, usize n) {
        for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
    };
    mix(name, std::strlen(name));
    mix(mesh, std::strlen(mesh));
    const i32 mm[3] = {static_cast<i32>(std::lround(position.x * 1000.0f)), static_cast<i32>(std::lround(position.y * 1000.0f)), static_cast<i32>(std::lround(position.z * 1000.0f))};
    mix(mm, sizeof mm);
    return h;
}

namespace {

struct CellKey {
    i32 c[3];
    bool operator<(const CellKey& o) const { return std::lexicographical_compare(c, c + 3, o.c, o.c + 3); }
};

// One source simplified, moved to world space and appended to the proxy (only the vertices it still uses).
void append_merged(const HlodSource& s, f32 ratio, HlodProxy& proxy) {
    std::vector<u32> kept(s.index_count);
    render::MeshData mesh;
    mesh.vertices = const_cast<rhi::MeshVertex*>(s.vertices);  // simplify only reads
    mesh.indices = const_cast<u32*>(s.indices);
    mesh.vertex_count = s.vertex_count;
    mesh.index_count = s.index_count;
    u32 count = s.index_count > 6 ? asset::simplify_mesh(mesh, ratio, kept.data()) : 0;
    if (count < 3) {  // too small to simplify (or it would vanish): keep it whole
        std::copy(s.indices, s.indices + s.index_count, kept.begin());
        count = s.index_count;
    }
    const Mat4 inv = affine_inverse(s.world);
    const Vec3 c0{s.world.at(0, 0), s.world.at(1, 0), s.world.at(2, 0)}, c1{s.world.at(0, 1), s.world.at(1, 1), s.world.at(2, 1)}, c2{s.world.at(0, 2), s.world.at(1, 2), s.world.at(2, 2)};
    const bool mirrored = dot(cross(c0, c1), c2) < 0.0f;
    std::vector<u32> remap(s.vertex_count, 0xFFFFFFFFu);
    for (u32 i = 0; i < count; ++i) {
        u32& slot = remap[kept[i]];
        if (slot == 0xFFFFFFFFu) {
            slot = static_cast<u32>(proxy.vertices.size());
            rhi::MeshVertex v = s.vertices[kept[i]];
            const Vec3 p = transform_point(s.world, {v.position[0], v.position[1], v.position[2]});
            const f32* n = v.normal;  // inverse transpose: right under non-uniform scale
            const Vec3 w = normalize(Vec3{inv.at(0, 0) * n[0] + inv.at(1, 0) * n[1] + inv.at(2, 0) * n[2], inv.at(0, 1) * n[0] + inv.at(1, 1) * n[1] + inv.at(2, 1) * n[2],
                                          inv.at(0, 2) * n[0] + inv.at(1, 2) * n[1] + inv.at(2, 2) * n[2]});
            v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
            v.normal[0] = w.x, v.normal[1] = w.y, v.normal[2] = w.z;
            v.lightmap_uv[0] = v.lightmap_uv[1] = 0.0f;
            proxy.vertices.push_back(v);
        }
        proxy.indices.push_back(slot);
    }
    if (mirrored) {  // a negative scale turns the triangles inside out: flip them back
        for (usize i = proxy.indices.size() - count; i + 2 < proxy.indices.size(); i += 3) std::swap(proxy.indices[i + 1], proxy.indices[i + 2]);
    }
}

} // namespace

void build_hlod(const std::vector<HlodSource>& sources, f32 cell_size, f32 ratio, HlodData& out) {
    out = HlodData{};
    out.cell_size = cell_size;
    if (cell_size <= 0.0f) return;
    std::map<CellKey, std::vector<const HlodSource*>> grid;
    for (const HlodSource& s : sources) {
        const CellKey k{{static_cast<i32>(std::floor(s.world.at(0, 3) / cell_size)), static_cast<i32>(std::floor(s.world.at(1, 3) / cell_size)),
                         static_cast<i32>(std::floor(s.world.at(2, 3) / cell_size))}};
        grid[k].push_back(&s);
    }
    for (const auto& entry : grid) {
        const std::vector<const HlodSource*>& members = entry.second;
        if (members.size() < 2) continue;
        HlodCell cell;
        for (int a = 0; a < 3; ++a) {
            cell.lo[a] = 1e30f;
            cell.hi[a] = -1e30f;
        }
        for (const HlodSource* s : members) {
            auto same = [s](const HlodProxy& p) { return std::strcmp(p.material, s->material) == 0 && std::memcmp(p.color, s->color, sizeof p.color) == 0; };
            auto it = std::find_if(cell.proxies.begin(), cell.proxies.end(), same);
            if (it == cell.proxies.end()) {
                cell.proxies.emplace_back();
                it = cell.proxies.end() - 1;
                std::snprintf(it->material, sizeof it->material, "%s", s->material);
                std::memcpy(it->color, s->color, sizeof it->color);
            }
            append_merged(*s, ratio, *it);
            cell.members.push_back(s->key);
        }
        for (const HlodProxy& p : cell.proxies) {
            for (const rhi::MeshVertex& v : p.vertices) {
                for (int a = 0; a < 3; ++a) {
                    cell.lo[a] = std::min(cell.lo[a], v.position[a]);
                    cell.hi[a] = std::max(cell.hi[a], v.position[a]);
                }
            }
        }
        out.cells.push_back(std::move(cell));
    }
    out.index();
}

bool hlod_cell_far(const HlodCell& cell, Vec3 eye, f32 distance) {
    const f32 e[3] = {eye.x, eye.y, eye.z};
    f32 d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 d = std::max({cell.lo[a] - e[a], e[a] - cell.hi[a], 0.0f});
        d2 += d * d;
    }
    return d2 > distance * distance;
}

bool gather_hlod_sources(const world::World& w, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator,
                         const std::function<bool(const world::StaticMesh&)>& allow, HlodStorage& storage, std::vector<HlodSource>& out) {
    out.clear();
    const reflect::TypeInfo& type = reflect::type_of<world::StaticMesh>();
    for (u32 i = 0; i < w.count(type); ++i) {
        const world::Entity e = w.owner(type, i);
        const auto* m = static_cast<const world::StaticMesh*>(w.at(type, i));
        const bool moves = w.get<world::RigidBody>(e) || w.get<world::Mover>(e) || w.get<world::Vehicle>(e) || w.get<world::Transport>(e);
        if (moves || w.hidden_in_hierarchy(e) || m->color.a < 0.999f || m->max_draw_distance > 0.0f || (allow && !allow(*m))) continue;
        f32 tiling = 1.0f;  // Box UV: the material's tiles per metre
        if (m->material[0]) {  // only plain opaque Materials (instances and graphs are left as they are)
            asset::Guid g;
            const asset::AssetRecord* r = asset::parse_guid(m->material, g) ? registry.find(g) : nullptr;
            char path[300];
            render::Material material;
            if (!r) continue;
            registry.source_path(*r, path, sizeof path);
            if (!asset::load_data_asset(path, material) || material.blend_mode != 0) continue;
            tiling = material.box_uv_tiling;
        }
        render::MeshData data;
        if (!load_level_mesh(m->mesh, registry, data_dir, allocator, data) || data.index_count < 3) continue;
        const Mat4 world_matrix = w.world_matrix(e);
        if (m->box_uv) {
            render::box_project_uv(data, tiling * length(Vec3{world_matrix.at(0, 0), world_matrix.at(1, 0), world_matrix.at(2, 0)}),
                                   tiling * length(Vec3{world_matrix.at(0, 1), world_matrix.at(1, 1), world_matrix.at(2, 1)}),
                                   tiling * length(Vec3{world_matrix.at(0, 2), world_matrix.at(1, 2), world_matrix.at(2, 2)}));
        }
        storage.vertices.emplace_back(data.vertices, data.vertices + data.vertex_count);
        storage.indices.emplace_back(data.indices, data.indices + data.index_count);
        const u32 vertex_count = data.vertex_count, index_count = data.index_count;
        render::free_mesh(allocator, data);  // copied above
        HlodSource s;
        s.key = hlod_member_key(w.name(e), m->mesh, {world_matrix.at(0, 3), world_matrix.at(1, 3), world_matrix.at(2, 3)});
        s.world = world_matrix;
        std::snprintf(s.material, sizeof s.material, "%s", m->material);
        s.color[0] = m->color.r;
        s.color[1] = m->color.g;
        s.color[2] = m->color.b;
        s.color[3] = m->color.a;
        s.vertices = storage.vertices.back().data();
        s.vertex_count = vertex_count;
        s.indices = storage.indices.back().data();
        s.index_count = index_count;
        out.push_back(s);
    }
    return !out.empty();
}

std::string hlod_path(const char* level_path) {
    const std::filesystem::path p(level_path);
    return (p.parent_path() / (p.stem().string() + "_BuiltData.lsdhlod")).generic_string();
}

namespace {
constexpr char kMagic[8] = {'L', 'S', 'D', 'H', 'L', 'O', 'D', '1'};

template <class T>
bool put(FILE* f, const T* p, usize n = 1) {
    return n == 0 || std::fwrite(p, sizeof(T), n, f) == n;
}
template <class T>
bool get(FILE* f, T* p, usize n = 1) {
    return n == 0 || std::fread(p, sizeof(T), n, f) == n;
}
} // namespace

bool save_hlod(const char* path, const HlodData& d) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const u32 cells = static_cast<u32>(d.cells.size());
    bool ok = put(f, kMagic, 8) && put(f, &d.cell_size) && put(f, &cells);
    for (u32 c = 0; ok && c < cells; ++c) {
        const HlodCell& cell = d.cells[c];
        const u32 counts[2] = {static_cast<u32>(cell.members.size()), static_cast<u32>(cell.proxies.size())};
        ok = put(f, cell.lo, 3) && put(f, cell.hi, 3) && put(f, counts, 2) && put(f, cell.members.data(), counts[0]);
        for (u32 p = 0; ok && p < counts[1]; ++p) {
            const HlodProxy& proxy = cell.proxies[p];
            const u32 n[2] = {static_cast<u32>(proxy.vertices.size()), static_cast<u32>(proxy.indices.size())};
            ok = put(f, proxy.material, 40) && put(f, proxy.color, 4) && put(f, n, 2) && put(f, proxy.vertices.data(), n[0]) && put(f, proxy.indices.data(), n[1]);
        }
    }
    return std::fclose(f) == 0 && ok;
}

bool load_hlod(const char* path, HlodData& out) {
    out = HlodData{};
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    u32 cells = 0;
    bool ok = get(f, magic, 8) && std::memcmp(magic, kMagic, 8) == 0 && get(f, &out.cell_size) && get(f, &cells) && cells < 1000000;
    out.cells.resize(ok ? cells : 0);
    for (u32 c = 0; ok && c < cells; ++c) {
        HlodCell& cell = out.cells[c];
        u32 counts[2] = {};
        ok = get(f, cell.lo, 3) && get(f, cell.hi, 3) && get(f, counts, 2) && counts[0] < 10000000 && counts[1] < 100000;
        if (!ok) break;
        cell.members.resize(counts[0]);
        cell.proxies.resize(counts[1]);
        ok = get(f, cell.members.data(), counts[0]);
        for (u32 p = 0; ok && p < counts[1]; ++p) {
            HlodProxy& proxy = cell.proxies[p];
            u32 n[2] = {};
            ok = get(f, proxy.material, 40) && get(f, proxy.color, 4) && get(f, n, 2) && n[0] < 100000000 && n[1] < 400000000;
            if (!ok) break;
            proxy.vertices.resize(n[0]);
            proxy.indices.resize(n[1]);
            ok = get(f, proxy.vertices.data(), n[0]) && get(f, proxy.indices.data(), n[1]);
            proxy.material[39] = 0;
            for (u32 k = 0; ok && k < n[1]; ++k) ok = proxy.indices[k] < n[0];  // a damaged file must not index past its vertices
        }
    }
    std::fclose(f);
    if (!ok) out = HlodData{};
    out.index();
    return ok;
}

} // namespace lsd::editor
