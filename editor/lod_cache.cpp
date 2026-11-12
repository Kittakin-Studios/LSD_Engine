#include "lod_cache.h"

#include "lsd/asset/mesh_import.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace lsd::editor {

namespace {

struct LodFileHeader {
    char magic[4];
    u32 version;
    i64 compiled_time;
    u32 index_count, lod_count;
    f32 sizes[render::kMaxLods];
    u32 counts[render::kMaxLods - 1];
};
constexpr char kLodMagic[4] = {'L', 'S', 'D', 'L'};
constexpr u32 kLodVersion = 1;  // bump when render::build_lods changes what it makes

bool read_header(FILE* f, i64 compiled_time, const f32 sizes[render::kMaxLods], LodFileHeader& h) {
    return std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kLodMagic, 4) == 0 && h.version == kLodVersion && h.compiled_time == compiled_time &&
           h.lod_count >= 1 && h.lod_count <= render::kMaxLods && std::memcmp(h.sizes, sizes, sizeof h.sizes) == 0;
}

i64 file_time(const char* path) {
    std::error_code error;
    return static_cast<i64>(std::filesystem::last_write_time(path, error).time_since_epoch().count());
}

} // namespace

bool read_lod_cache(const char* path, i64 compiled_time, const render::MeshData& mesh, const f32 sizes[render::kMaxLods], u32* const out[render::kMaxLods - 1],
                    LodSet& lods) {
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    LodFileHeader h{};
    bool ok = read_header(f, compiled_time, sizes, h) && h.index_count == mesh.index_count;
    for (u32 k = 1; ok && k < h.lod_count; ++k) {
        ok = h.counts[k - 1] <= mesh.index_count && std::fread(out[k - 1], sizeof(u32), h.counts[k - 1], f) == h.counts[k - 1];
        for (u32 i = 0; ok && i < h.counts[k - 1]; ++i) ok = out[k - 1][i] < mesh.vertex_count;  // a damaged file must not index past the vertices
    }
    std::fclose(f);
    if (!ok) return false;
    lods.lod_count = h.lod_count;
    std::memcpy(lods.counts, h.counts, sizeof lods.counts);
    return true;
}

void write_lod_cache(const char* path, i64 compiled_time, const render::MeshData& mesh, const f32 sizes[render::kMaxLods], u32* const out[render::kMaxLods - 1],
                     const LodSet& lods) {
    if (!path) return;
    LodFileHeader h{};
    std::memcpy(h.magic, kLodMagic, 4);
    h.version = kLodVersion;
    h.compiled_time = compiled_time;
    h.index_count = mesh.index_count;
    h.lod_count = lods.lod_count;
    std::memcpy(h.sizes, sizes, sizeof h.sizes);
    std::memcpy(h.counts, lods.counts, sizeof h.counts);
    const std::string temp = std::string(path) + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return;
    bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
    for (u32 k = 1; ok && k < h.lod_count; ++k) ok = std::fwrite(out[k - 1], sizeof(u32), h.counts[k - 1], f) == h.counts[k - 1];
    ok = std::fclose(f) == 0 && ok;
    std::error_code error;
    if (ok) std::filesystem::rename(temp, path, error);  // a disk full or a reader holding it: built again next time
    if (!ok || error) std::filesystem::remove(temp, error);
}

bool lod_cache_current(const char* path, i64 compiled_time, const f32 sizes[render::kMaxLods]) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    LodFileHeader h{};
    const bool ok = read_header(f, compiled_time, sizes, h);
    std::fclose(f);
    return ok;
}

bool warm_lod_cache(const char* compiled, const f32 sizes[render::kMaxLods], Allocator& allocator) {
    const std::string path = std::string(compiled) + ".lod";
    const i64 time = file_time(compiled);
    if (lod_cache_current(path.c_str(), time, sizes)) return true;
    render::MeshData authored[asset::kMaxAuthoredLods];
    u32 authored_count = 0;
    if (asset::load_compiled_lods(compiled, allocator, authored, authored_count) && authored_count) {  // drawn with the file's own LODs
        for (u32 l = 0; l < authored_count; ++l) render::free_mesh(allocator, authored[l]);
        return true;
    }
    render::MeshData mesh;
    if (!asset::load_compiled_mesh(compiled, allocator, mesh)) return false;
    u32* indices = static_cast<u32*>(allocator.allocate(sizeof(u32) * mesh.index_count * (render::kMaxLods - 1), alignof(u32)));
    u32* out[render::kMaxLods - 1];
    for (u32 k = 0; k + 1 < render::kMaxLods; ++k) out[k] = indices + static_cast<usize>(k) * mesh.index_count;
    LodSet lods;
    lods.lod_count = render::build_lods(mesh, sizes, out, lods.counts);
    write_lod_cache(path.c_str(), time, mesh, sizes, out, lods);
    allocator.deallocate(indices);
    render::free_mesh(allocator, mesh);
    return true;
}

} // namespace lsd::editor
