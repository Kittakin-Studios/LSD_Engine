#include "fracture_cache.h"

#include "lsd/foundation/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::editor {

namespace {

constexpr u32 kMagic = 0x31435246;  // "FRC1"

struct Hash {
    u64 h = 1469598103934665603ull;  // FNV-1a
    void add(const void* data, usize bytes) {
        for (usize i = 0; i < bytes; ++i) h = (h ^ static_cast<const u8*>(data)[i]) * 1099511628211ull;
    }
    void add(const char* text) { add(text, std::strlen(text) + 1); }
    template <typename T> void value(const T& v) { add(&v, sizeof v); }
};

template <typename T> void put(FILE* f, const std::vector<T>& v) {
    const u32 n = static_cast<u32>(v.size());
    std::fwrite(&n, sizeof n, 1, f);
    if (n) std::fwrite(v.data(), sizeof(T), n, f);
}
template <typename T> bool take(FILE* f, std::vector<T>& v) {
    u32 n = 0;
    if (std::fread(&n, sizeof n, 1, f) != 1 || n > (1u << 26)) return false;
    v.resize(n);
    return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
}

bool save(const FracturedMesh& m, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const u32 count = static_cast<u32>(m.pieces.size());
    std::fwrite(&kMagic, sizeof kMagic, 1, f);
    std::fwrite(&count, sizeof count, 1, f);
    for (u32 p = 0; p < count; ++p) {
        const FracturedMesh::Piece& piece = m.pieces[p];
        std::fwrite(&piece.centre_of_mass, sizeof(Vec3), 1, f);
        std::fwrite(&piece.bounds_min, sizeof(Vec3), 1, f);
        std::fwrite(&piece.bounds_max, sizeof(Vec3), 1, f);
        std::fwrite(&piece.volume, sizeof(f32), 1, f);
        put(f, piece.hull);
        put(f, m.meshes[p].vertices);
        put(f, m.meshes[p].indices);
        put(f, m.meshes[p].sections);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool load(FracturedMesh& m, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    u32 magic = 0, count = 0;
    bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == kMagic && std::fread(&count, sizeof count, 1, f) == 1 && count < 100000;
    if (ok) {
        m.pieces.resize(count);
        m.meshes.resize(count);
    }
    for (u32 p = 0; ok && p < count; ++p) {
        FracturedMesh::Piece& piece = m.pieces[p];
        ok = std::fread(&piece.centre_of_mass, sizeof(Vec3), 1, f) == 1 && std::fread(&piece.bounds_min, sizeof(Vec3), 1, f) == 1 &&
             std::fread(&piece.bounds_max, sizeof(Vec3), 1, f) == 1 && std::fread(&piece.volume, sizeof(f32), 1, f) == 1 && take(f, piece.hull) &&
             take(f, m.meshes[p].vertices) && take(f, m.meshes[p].indices) && take(f, m.meshes[p].sections) &&
             m.meshes[p].sections.size() * 3 == m.meshes[p].indices.size();
        for (const u32 i : m.meshes[p].indices) ok = ok && i < m.meshes[p].vertices.size();
    }
    std::fclose(f);
    return ok;
}

// The triangles of `mesh` that are in `section`, as a mesh of their own.
render::MeshData section_mesh(Allocator& allocator, const render::CutMesh& mesh, u16 section, std::vector<u32>& remap) {
    remap.assign(mesh.vertices.size(), ~0u);
    u32 vertices = 0, indices = 0;
    for (u32 t = 0; t < mesh.triangle_count(); ++t) {
        if (mesh.sections[t] != section) continue;
        indices += 3;
        for (u32 k = 0; k < 3; ++k) {
            u32& r = remap[mesh.indices[t * 3 + k]];
            if (r == ~0u) r = vertices++;
        }
    }
    render::MeshData data = render::allocate_mesh(allocator, vertices, indices);
    for (u32 v = 0; v < remap.size(); ++v) {
        if (remap[v] != ~0u) data.vertices[remap[v]] = mesh.vertices[v];
    }
    u32 at = 0;
    for (u32 t = 0; t < mesh.triangle_count(); ++t) {
        if (mesh.sections[t] != section) continue;
        for (u32 k = 0; k < 3; ++k) data.indices[at++] = remap[mesh.indices[t * 3 + k]];
    }
    return data;
}

} // namespace

void FracturedMesh::key(u32 piece, u16 section, char (&out)[40]) const { std::snprintf(out, sizeof out, "Fracture:%s:%u:%u", id, piece, static_cast<u32>(section)); }

const FracturedMesh* FractureCache::get(const world::StaticMesh& mesh, const world::Destructible& d, Vec3 scale) {
    auto cm = [](f32 v) { return std::fmax(std::round(std::fabs(v) * 100.0f) / 100.0f, 0.01f); };
    scale = {cm(scale.x), cm(scale.y), cm(scale.z)};
    // The source: the mesh whole, or per material slot (its parts, placed) when the slots carry the materials.
    struct Source {
        std::string key;
        Mat4 local;
        std::string material;
    };
    std::vector<Source> sources;
    const std::vector<MeshCache::SlotPart>* parts = mesh.material[0] ? nullptr : meshes_.slot_parts(mesh.mesh);
    if (parts && !parts->empty()) {
        for (const MeshCache::SlotPart& part : *parts) sources.push_back({part.key, part.local, part.material});
    } else {
        sources.push_back({mesh.mesh, Mat4{}, mesh.material});
    }
    std::vector<render::CutMesh> loaded(sources.size());
    Hash hash;
    hash.value(kMagic);
    for (usize k = 0; k < sources.size(); ++k) {
        std::vector<u32> indices;
        i64 stamp = 0;
        if (!meshes_.load_cpu(sources[k].key.c_str(), loaded[k].vertices, indices, &stamp)) return nullptr;
        loaded[k].indices = std::move(indices);
        hash.add(sources[k].key.c_str());
        hash.add(sources[k].material.c_str());
        hash.value(stamp);
        hash.value(sources[k].local);
    }
    hash.value(d.pieces), hash.value(d.seed), hash.value(d.cluster), hash.value(d.cluster_point), hash.value(d.cluster_radius), hash.value(d.inside_uv_scale);
    hash.add(d.inside_material);
    hash.value(scale);
    char id[13];
    std::snprintf(id, sizeof id, "%012llx", static_cast<unsigned long long>(hash.h & 0xFFFFFFFFFFFFull));
    if (const auto it = cache_.find(id); it != cache_.end()) return it->second->pieces.empty() ? nullptr : it->second.get();

    auto made = std::make_unique<FracturedMesh>();
    FracturedMesh& f = *made;
    std::snprintf(f.id, sizeof f.id, "%s", id);
    for (const Source& s : sources) f.materials.push_back(s.material);
    f.materials.push_back(d.inside_material[0] ? std::string(d.inside_material) : f.materials.front());
    char dir[300], path[320];
    std::snprintf(dir, sizeof dir, "%s/Fracture", meshes_.data_dir());
    std::snprintf(path, sizeof path, "%s/%s.bin", dir, id);
    if (!load(f, path)) {
        f.pieces.clear();
        f.meshes.clear();
        render::CutMesh whole;
        for (usize k = 0; k < sources.size(); ++k) {
            const u32 base = static_cast<u32>(whole.vertices.size());
            for (rhi::MeshVertex v : loaded[k].vertices) {
                const Vec3 p = mul(transform_point(sources[k].local, {v.position[0], v.position[1], v.position[2]}), scale);  // cut at the object's size
                Vec3 n = transform_vector(sources[k].local, {v.normal[0], v.normal[1], v.normal[2]});
                n = Vec3{n.x / scale.x, n.y / scale.y, n.z / scale.z};
                n = length(n) > 0.0f ? normalize(n) : Vec3{0, 0, 1};
                v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
                v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
                whole.vertices.push_back(v);
            }
            for (const u32 i : loaded[k].indices) whole.indices.push_back(base + i);
            whole.sections.resize(whole.indices.size() / 3, static_cast<u16>(k));
        }
        render::FractureSettings settings;
        settings.pieces = d.pieces < 2 ? 2 : d.pieces;
        settings.seed = d.seed;
        settings.cluster = d.cluster;
        settings.cluster_point = mul(d.cluster_point, scale);
        settings.cluster_radius = d.cluster_radius;
        settings.uv_scale = d.inside_uv_scale;
        render::FractureStats stats;
        std::vector<render::FracturePiece> pieces = render::fracture_voronoi(whole, settings, &stats);
        const u16 inside = static_cast<u16>(f.materials.size() - 1);
        const Vec3 unscale{1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z};  // back into the mesh's own space
        for (render::FracturePiece& p : pieces) {
            for (u16& s : p.mesh.sections) s = s == render::kInsideSection ? inside : s;
            for (rhi::MeshVertex& v : p.mesh.vertices) {
                for (int c = 0; c < 3; ++c) v.position[c] *= (&unscale.x)[c];
                Vec3 n{v.normal[0] * scale.x, v.normal[1] * scale.y, v.normal[2] * scale.z};
                n = length(n) > 0.0f ? normalize(n) : Vec3{0, 0, 1};
                v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
            }
            FracturedMesh::Piece piece;
            piece.centre_of_mass = mul(p.centre_of_mass, unscale);
            piece.bounds_min = mul(p.bounds_min, unscale);
            piece.bounds_max = mul(p.bounds_max, unscale);
            piece.volume = p.volume * unscale.x * unscale.y * unscale.z;
            for (Vec3& h : p.hull) h = mul(h, unscale);
            piece.hull = std::move(p.hull);
            f.pieces.push_back(std::move(piece));
            f.meshes.push_back(std::move(p.mesh));
        }
        f.build_ms = stats.milliseconds;
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (!f.pieces.empty() && !save(f, path)) LSD_LOG_WARN("fracture", "Could not write %s", path);
        LSD_LOG_INFO("fracture", "%s: %u triangles into %zu pieces in %.0f ms (%u cuts)", mesh.mesh, whole.triangle_count(), f.pieces.size(), stats.milliseconds, stats.cuts);
    }
    // Which sections each piece has, the bounds, and its meshes on the GPU.
    f.bounds_min = {1e30f, 1e30f, 1e30f};
    f.bounds_max = {-1e30f, -1e30f, -1e30f};
    Allocator& heap = default_allocator();
    std::vector<u32> remap;
    for (u32 p = 0; p < f.pieces.size(); ++p) {
        FracturedMesh::Piece& piece = f.pieces[p];
        f.bounds_min = vmin(f.bounds_min, piece.bounds_min);
        f.bounds_max = vmax(f.bounds_max, piece.bounds_max);
        piece.sections.clear();
        for (u16 s = 0; s < f.materials.size(); ++s) {
            render::MeshData data = section_mesh(heap, f.meshes[p], s, remap);
            if (data.index_count) {
                char key[40];
                f.key(p, s, key);
                meshes_.set_generated(key, data);
                piece.sections.push_back(s);
            }
            render::free_mesh(heap, data);
        }
    }
    if (f.pieces.empty()) LSD_LOG_WARN("fracture", "%s: fracture gave no pieces", mesh.mesh);
    const FracturedMesh* out = f.pieces.empty() ? nullptr : made.get();
    cache_[id] = std::move(made);
    return out;
}

bool FractureCache::build_rest(const FracturedMesh& f, const std::vector<u32>& pieces, u16 section, const char* key) {
    render::CutMesh all;
    for (const u32 p : pieces) {
        const render::CutMesh& m = f.meshes[p];
        const u32 base = static_cast<u32>(all.vertices.size());
        all.vertices.insert(all.vertices.end(), m.vertices.begin(), m.vertices.end());
        for (const u32 i : m.indices) all.indices.push_back(base + i);
        all.sections.insert(all.sections.end(), m.sections.begin(), m.sections.end());
    }
    Allocator& heap = default_allocator();
    std::vector<u32> remap;
    render::MeshData data = section_mesh(heap, all, section, remap);
    const bool any = data.index_count > 0;
    if (any) meshes_.set_generated(key, data);
    render::free_mesh(heap, data);
    return any;
}

FractureCache& fracture_cache(MeshCache& meshes) {
    static FractureCache* cache = new FractureCache(meshes);  // shortcut: never freed (one MeshCache per process); its GPU meshes go with the MeshCache
    return *cache;
}

} // namespace lsd::editor
