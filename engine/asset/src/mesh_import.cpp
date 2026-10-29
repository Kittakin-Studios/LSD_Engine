#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/array.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/log.h"

#include <cgltf.h>
#include <meshoptimizer.h>
#include <ufbx.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::asset {

namespace {

constexpr char kMagic[4] = {'L', 'S', 'D', 'M'};
constexpr u32 kVersion = 1;

struct CompiledHeader {
    char magic[4];
    u32 version;
    u64 source_size;
    i64 source_time;
    u32 settings_hash;
    u32 vertex_count;
    u32 index_count;
    f32 bounds_min[3];
    f32 bounds_max[3];
};

bool has_extension(const char* path, const char* ext) {
    const usize n = std::strlen(path), e = std::strlen(ext);
    if (n < e) return false;
    for (usize i = 0; i < e; ++i) {
        if (std::tolower(static_cast<unsigned char>(path[n - e + i])) != ext[i]) return false;
    }
    return true;
}

rhi::MeshVertex make_vertex(f32 px, f32 py, f32 pz, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    const f32 inv = len > 0.0f ? 1.0f / len : 0.0f;
    return rhi::MeshVertex{{px, py, pz}, {nx * inv, ny * inv, nz * inv}, {u, v}};
}

// FBX: ufbx converts axes to Z-up right-handed and units to metres for us.
bool load_fbx(const char* path, f32 scale, Allocator& allocator, Array<rhi::MeshVertex>& soup, char* error, usize error_capacity) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    ufbx_error ufbx_err{};
    ufbx_scene* scene = ufbx_load_file(path, &opts, &ufbx_err);
    if (!scene) {
        std::snprintf(error, error_capacity, "FBX: %s", ufbx_err.description.data);
        return false;
    }
    Array<u32> tri(allocator);
    for (usize n = 0; n < scene->nodes.count; ++n) {
        const ufbx_node* node = scene->nodes.data[n];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;
        const ufbx_matrix to_world = node->geometry_to_world;
        const ufbx_matrix normal_to_world = ufbx_matrix_for_normals(&to_world);
        tri.resize(mesh->max_face_triangles * 3);
        for (usize f = 0; f < mesh->faces.count; ++f) {
            const u32 count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[f]);
            for (u32 i = 0; i < count * 3; ++i) {
                const u32 index = tri[i];
                const ufbx_vec3 p = ufbx_transform_position(&to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, index));
                const ufbx_vec3 nrm = mesh->vertex_normal.exists
                    ? ufbx_transform_direction(&normal_to_world, ufbx_get_vertex_vec3(&mesh->vertex_normal, index))
                    : ufbx_vec3{0, 0, 1};
                ufbx_vec2 uv{0, 0};
                if (mesh->vertex_uv.exists) uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                soup.push_back(make_vertex(static_cast<f32>(p.x) * scale, static_cast<f32>(p.y) * scale, static_cast<f32>(p.z) * scale,
                                           static_cast<f32>(nrm.x), static_cast<f32>(nrm.y), static_cast<f32>(nrm.z),
                                           static_cast<f32>(uv.x), 1.0f - static_cast<f32>(uv.y)));
            }
        }
    }
    ufbx_free_scene(scene);
    return true;
}

// glTF is Y-up right-handed in metres: rotate +90 degrees about X so Y becomes Z.
bool load_gltf(const char* path, f32 scale, Array<rhi::MeshVertex>& soup, char* error, usize error_capacity) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        std::snprintf(error, error_capacity, "glTF: could not read %s", path);
        cgltf_free(data);
        return false;
    }
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        cgltf_float m[16];  // column-major world matrix
        cgltf_node_transform_world(&node, m);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
            }
            if (!pos) continue;
            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            for (cgltf_size i = 0; i < count; ++i) {
                const cgltf_size v = prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i;
                cgltf_float lp[3] = {0, 0, 0}, ln[3] = {0, 0, 1}, t[2] = {0, 0};
                cgltf_accessor_read_float(pos, v, lp, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, ln, 3);
                if (uv) cgltf_accessor_read_float(uv, v, t, 2);
                // World transform (column-major), then Y-up -> Z-up: (x, y, z) -> (x, -z, y).
                const f32 wx = m[0] * lp[0] + m[4] * lp[1] + m[8] * lp[2] + m[12];
                const f32 wy = m[1] * lp[0] + m[5] * lp[1] + m[9] * lp[2] + m[13];
                const f32 wz = m[2] * lp[0] + m[6] * lp[1] + m[10] * lp[2] + m[14];
                // shortcut: normals use the upper 3x3 directly; wrong only for non-uniform node scale.
                const f32 nx = m[0] * ln[0] + m[4] * ln[1] + m[8] * ln[2];
                const f32 ny = m[1] * ln[0] + m[5] * ln[1] + m[9] * ln[2];
                const f32 nz = m[2] * ln[0] + m[6] * ln[1] + m[10] * ln[2];
                soup.push_back(make_vertex(wx * scale, -wz * scale, wy * scale, nx, -nz, ny, t[0], t[1]));
            }
        }
    }
    cgltf_free(data);
    return true;
}

} // namespace

bool import_mesh(const char* path, f32 scale, Allocator& allocator, render::MeshData& out, char* error, usize error_capacity) {
    error[0] = '\0';
    Array<rhi::MeshVertex> soup(allocator);  // unindexed triangle list
    bool ok = false;
    if (has_extension(path, ".fbx")) ok = load_fbx(path, scale, allocator, soup, error, error_capacity);
    else if (has_extension(path, ".gltf") || has_extension(path, ".glb")) ok = load_gltf(path, scale, soup, error, error_capacity);
    else std::snprintf(error, error_capacity, "Unsupported file type: %s", path);
    if (!ok) return false;
    if (soup.empty()) {
        std::snprintf(error, error_capacity, "No triangles found in %s", path);
        return false;
    }

    // Weld identical vertices into an index buffer, then reorder triangles for the vertex cache.
    const usize count = soup.size();
    Array<u32> remap(allocator);
    remap.resize(count);
    const usize unique = meshopt_generateVertexRemap(remap.data(), nullptr, count, soup.data(), count, sizeof(rhi::MeshVertex));
    out = render::allocate_mesh(allocator, static_cast<u32>(unique), static_cast<u32>(count));
    meshopt_remapVertexBuffer(out.vertices, soup.data(), count, sizeof(rhi::MeshVertex), remap.data());
    meshopt_remapIndexBuffer(out.indices, nullptr, count, remap.data());
    meshopt_optimizeVertexCache(out.indices, out.indices, count, unique);
    return true;
}

MeshBounds compute_bounds(const render::MeshData& mesh) {
    MeshBounds b;
    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        for (int k = 0; k < 3; ++k) {
            const f32 v = mesh.vertices[i].position[k];
            if (i == 0 || v < b.min[k]) b.min[k] = v;
            if (i == 0 || v > b.max[k]) b.max[k] = v;
        }
    }
    return b;
}

bool save_compiled_mesh(const char* path, const render::MeshData& mesh, const CompiledMeshSource& source) {
    CompiledHeader h{};
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    h.version = kVersion;
    h.source_size = source.source_size;
    h.source_time = source.source_time;
    h.settings_hash = source.settings_hash;
    h.vertex_count = mesh.vertex_count;
    h.index_count = mesh.index_count;
    const MeshBounds b = compute_bounds(mesh);
    std::memcpy(h.bounds_min, b.min, sizeof b.min);
    std::memcpy(h.bounds_max, b.max, sizeof b.max);

    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    bool ok = std::fwrite(&h, sizeof h, 1, file) == 1 &&
              std::fwrite(mesh.vertices, sizeof(rhi::MeshVertex), mesh.vertex_count, file) == mesh.vertex_count &&
              std::fwrite(mesh.indices, sizeof(u32), mesh.index_count, file) == mesh.index_count;
    ok = std::fclose(file) == 0 && ok;
    return ok;
}

static bool read_header(FILE* file, CompiledHeader& h) {
    return std::fread(&h, sizeof h, 1, file) == 1 && std::memcmp(h.magic, kMagic, sizeof kMagic) == 0 && h.version == kVersion;
}

bool compiled_mesh_up_to_date(const char* path, const CompiledMeshSource& source) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    CompiledHeader h{};
    const bool ok = read_header(file, h) && h.source_size == source.source_size && h.source_time == source.source_time &&
                    h.settings_hash == source.settings_hash;
    std::fclose(file);
    return ok;
}

bool load_compiled_mesh(const char* path, Allocator& allocator, render::MeshData& out, MeshBounds* bounds) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    CompiledHeader h{};
    bool ok = read_header(file, h) && h.vertex_count > 0 && h.index_count > 0;
    if (ok) {
        out = render::allocate_mesh(allocator, h.vertex_count, h.index_count);
        ok = std::fread(out.vertices, sizeof(rhi::MeshVertex), h.vertex_count, file) == h.vertex_count &&
             std::fread(out.indices, sizeof(u32), h.index_count, file) == h.index_count;
        for (u32 i = 0; ok && i < h.index_count; ++i) ok = out.indices[i] < h.vertex_count;  // never trust file data
        if (!ok) render::free_mesh(allocator, out);
    }
    std::fclose(file);
    if (ok && bounds) {
        std::memcpy(bounds->min, h.bounds_min, sizeof h.bounds_min);
        std::memcpy(bounds->max, h.bounds_max, sizeof h.bounds_max);
    }
    return ok;
}

bool compile_mesh_asset(const AssetRegistry& registry, const AssetRecord& record, const char* data_dir, Allocator& allocator,
                        char* compiled_path, usize compiled_path_capacity) {
    char guid[33];
    to_string(record.guid, guid);
    std::snprintf(compiled_path, compiled_path_capacity, "%s/%s.lsdmesh", data_dir, guid);

    AssetMeta meta;
    registry.load_meta(record, meta);
    CompiledMeshSource source{record.source_size, record.source_time, 0};
    char settings[64];
    std::snprintf(settings, sizeof settings, "scale=%.6g", static_cast<f64>(meta.import_scale));
    source.settings_hash = static_cast<u32>(hash64(settings));
    if (compiled_mesh_up_to_date(compiled_path, source)) return true;

    char path[300], error[256];
    registry.source_path(record, path, sizeof path);
    render::MeshData mesh;
    if (!import_mesh(path, meta.import_scale, allocator, mesh, error, sizeof error)) {
        LSD_LOG_ERROR("asset", "Import failed for %s: %s", record.path, error);
        return false;
    }
    std::error_code ignored;
    std::filesystem::create_directories(data_dir, ignored);
    const bool ok = save_compiled_mesh(compiled_path, mesh, source);
    LSD_LOG_INFO("asset", "Compiled %s: %u vertices, %u triangles", record.path, mesh.vertex_count, mesh.index_count / 3);
    render::free_mesh(allocator, mesh);
    return ok;
}

} // namespace lsd::asset
