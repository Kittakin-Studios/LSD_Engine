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
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace lsd::asset {

namespace {

constexpr char kMagic[4] = {'L', 'S', 'D', 'M'};
constexpr u32 kVersion = 2;  // 2: MeshVertex gained lightmap_uv

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

// A node's world matrix (engine space) as a Transform for the prefab, plus what a Transform cannot
// hold (shear from a non-uniformly scaled, rotated parent), returned for the part's vertices to keep:
// trs(part) * residual == world, so parts land exactly where the file puts them.
Mat4 split_node_transform(const Mat4& world, MeshPart& part) {
    decompose_trs(world, part.position, part.rotation, part.scale);
    return inverse(trs(part.position, part.rotation, part.scale)) * world;
}

Mat4 from_ufbx(const ufbx_matrix& u) {
    Mat4 m;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) m.at(r, c) = static_cast<f32>(u.cols[c].v[r]);
    }
    return m;
}

ufbx_matrix to_ufbx(const Mat4& m) {
    ufbx_matrix u{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) u.cols[c].v[r] = m.at(r, c);
    }
    return u;
}

// "Node" or, when the node has several parts, "Node_Material"; `fallback` when the node has no name.
// "Top/Middle/Node": the names from the scene's top node down to this one (unnamed ones as "Node").
template <typename NameOf, typename ParentOf, typename Node>
void node_path(char* out, usize capacity, const Node* node, NameOf name_of, ParentOf parent_of) {
    const Node* chain[32];
    u32 n = 0;
    for (const Node* at = node; at && n < 32; at = parent_of(at)) chain[n++] = at;
    usize len = 0;
    out[0] = '\0';
    while (n-- > 0) {
        const char* name = name_of(chain[n]);
        const int wrote = std::snprintf(out + len, capacity - len, "%s%s", len ? "/" : "", name && name[0] ? name : "Node");
        if (wrote < 0 || static_cast<usize>(wrote) >= capacity - len) return;
        len += static_cast<usize>(wrote);
    }
}

void part_name(MeshPart& part, const char* node, const char* fallback, bool several) {
    const char* base = node && node[0] ? node : fallback;
    if (several && part.material[0]) std::snprintf(part.name, sizeof part.name, "%s_%s", base, part.material);
    else std::snprintf(part.name, sizeof part.name, "%s", base);
}

// The file name of a texture path ("C:/art/brick.png" -> "brick.png").
void file_name(char* out, usize capacity, const char* path) {
    const char* name = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '/' || *c == '\\') name = c + 1;
    }
    std::snprintf(out, capacity, "%s", name);
}

// FBX: ufbx converts axes to Z-up right-handed and units to metres for us. A part is one material
// slot of one mesh node. Fills `soup` with the triangles of `part` (all when < 0) and / or `parts`.
bool load_fbx(const char* path, f32 scale, i32 part, Allocator& allocator, Array<rhi::MeshVertex>* soup, Array<MeshPart>* parts, char* error,
              usize error_capacity) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    opts.obj_axes = ufbx_axes_right_handed_y_up;  // OBJ has no axis info: Y-up in metres, as Blender and Maya write it
    opts.obj_unit_meters = 1.0f;
    ufbx_error ufbx_err{};
    ufbx_scene* scene = ufbx_load_file(path, &opts, &ufbx_err);
    if (!scene) {
        std::snprintf(error, error_capacity, "FBX: %s", ufbx_err.description.data);
        return false;
    }
    Array<u32> tri(allocator);
    i32 k = 0;
    for (usize n = 0; n < scene->nodes.count; ++n) {
        const ufbx_node* node = scene->nodes.data[n];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;
        // A single part stays in its node's space (the prefab places it); the whole file is baked together.
        MeshPart placed;
        const Mat4 residual = split_node_transform(from_ufbx(node->node_to_world), placed);
        const ufbx_matrix residual_u = to_ufbx(residual);
        const ufbx_matrix to_world = part >= 0 ? ufbx_matrix_mul(&residual_u, &node->geometry_to_node) : node->geometry_to_world;
        const ufbx_matrix normal_to_world = ufbx_matrix_for_normals(&to_world);
        tri.resize(mesh->max_face_triangles * 3);
        u32 used_parts = 0;
        for (usize m = 0; m < mesh->material_parts.count; ++m) used_parts += mesh->material_parts.data[m].num_triangles > 0 ? 1u : 0u;
        for (usize m = 0; m < mesh->material_parts.count; ++m) {
            const ufbx_mesh_part& mp = mesh->material_parts.data[m];
            if (mp.num_triangles == 0) continue;
            if (parts) {
                MeshPart info;
                // Per-instance materials live on the node, at the mesh's material indices.
                if (const ufbx_material* mat = m < node->materials.count ? node->materials.data[m] : nullptr) {
                    std::snprintf(info.material, sizeof info.material, "%s", mat->name.data);
                    const ufbx_vec4 c = mat->pbr.base_color.value_vec4;
                    const f32 factor = mat->pbr.base_factor.has_value ? static_cast<f32>(mat->pbr.base_factor.value_real) : 1.0f;
                    info.base_color[0] = static_cast<f32>(c.x) * factor;
                    info.base_color[1] = static_cast<f32>(c.y) * factor;
                    info.base_color[2] = static_cast<f32>(c.z) * factor;
                    info.metallic = static_cast<f32>(mat->pbr.metalness.value_real);
                    if (mat->pbr.roughness.has_value) info.roughness = static_cast<f32>(mat->pbr.roughness.value_real);
                    auto texture_file = [](const ufbx_texture* t, char* out, usize capacity) {
                        if (t) file_name(out, capacity, t->filename.length ? t->filename.data : t->relative_filename.data);
                    };
                    texture_file(mat->pbr.base_color.texture, info.texture, sizeof info.texture);
                    texture_file(mat->pbr.normal_map.texture, info.normal_texture, sizeof info.normal_texture);
                }
                part_name(info, node->name.data, mesh->name.data, used_parts > 1);
                node_path(info.node_path, sizeof info.node_path, node, [](const ufbx_node* x) { return x->name.data; },
                          [](const ufbx_node* x) -> const ufbx_node* { return x->parent && !x->parent->is_root ? x->parent : nullptr; });
                info.position = placed.position;
                info.rotation = placed.rotation;
                info.scale = placed.scale;
                parts->push_back(info);
            }
            if (soup && (part < 0 || part == k)) {
                for (usize f = 0; f < mp.face_indices.count; ++f) {
                    const u32 count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[mp.face_indices.data[f]]);
                    for (u32 i = 0; i < count * 3; ++i) {
                        const u32 index = tri[i];
                        const ufbx_vec3 p = ufbx_transform_position(&to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, index));
                        const ufbx_vec3 nrm = mesh->vertex_normal.exists
                            ? ufbx_transform_direction(&normal_to_world, ufbx_get_vertex_vec3(&mesh->vertex_normal, index))
                            : ufbx_vec3{{{0, 0, 1}}};
                        ufbx_vec2 uv{{{0, 0}}};
                        if (mesh->vertex_uv.exists) uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                        soup->push_back(make_vertex(static_cast<f32>(p.x) * scale, static_cast<f32>(p.y) * scale, static_cast<f32>(p.z) * scale,
                                                    static_cast<f32>(nrm.x), static_cast<f32>(nrm.y), static_cast<f32>(nrm.z),
                                                    static_cast<f32>(uv.x), 1.0f - static_cast<f32>(uv.y)));
                    }
                }
            }
            ++k;
        }
    }
    ufbx_free_scene(scene);
    return true;
}

// glTF is Y-up right-handed in metres: rotate +90 degrees about X so Y becomes Z. A part is one
// triangle primitive of one mesh node (each primitive has one material).
bool load_gltf(const char* path, f32 scale, i32 part, Array<rhi::MeshVertex>* soup, Array<MeshPart>* parts, char* error, usize error_capacity) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        std::snprintf(error, error_capacity, "glTF: could not read %s", path);
        cgltf_free(data);
        return false;
    }
    i32 k = 0;
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        cgltf_float m[16];  // column-major world matrix
        cgltf_node_transform_world(&node, m);
        Mat4 node_world;  // in engine space: C * world * C^-1, C turning Y-up into Z-up
        std::memcpy(node_world.m, m, sizeof m);
        Mat4 up, down;
        up.at(1, 1) = 0, up.at(1, 2) = -1, up.at(2, 1) = 1, up.at(2, 2) = 0;
        down.at(1, 1) = 0, down.at(1, 2) = 1, down.at(2, 1) = -1, down.at(2, 2) = 0;
        const Mat4 file_world = node_world;
        node_world = up * node_world * down;
        MeshPart placed;
        split_node_transform(node_world, placed);
        if (part >= 0) {  // a single part stays in its node's space (see FBX): C * m = trs^-1 * C * world
            const Mat4 local = down * inverse(trs(placed.position, placed.rotation, placed.scale)) * up * file_world;
            std::memcpy(m, local.m, sizeof m);
        }
        auto position_of = [](const cgltf_primitive& prim) -> const cgltf_accessor* {
            if (prim.type != cgltf_primitive_type_triangles) return nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) return prim.attributes[a].data;
            }
            return nullptr;
        };
        u32 used_parts = 0;
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) used_parts += position_of(node.mesh->primitives[p]) ? 1u : 0u;
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            const cgltf_accessor* pos = position_of(prim);
            if (!pos) continue;
            const i32 this_part = k++;
            if (parts) {
                MeshPart info;
                if (const cgltf_material* mat = prim.material) {
                    if (mat->name) std::snprintf(info.material, sizeof info.material, "%s", mat->name);
                    const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
                    std::memcpy(info.base_color, pbr.base_color_factor, sizeof info.base_color);
                    info.metallic = pbr.metallic_factor;
                    info.roughness = pbr.roughness_factor;
                    auto texture_file = [](const cgltf_texture* t, char* out, usize capacity) {
                        if (t && t->image && t->image->uri) file_name(out, capacity, t->image->uri);
                    };
                    texture_file(pbr.base_color_texture.texture, info.texture, sizeof info.texture);
                    texture_file(mat->normal_texture.texture, info.normal_texture, sizeof info.normal_texture);
                    texture_file(pbr.metallic_roughness_texture.texture, info.orm_texture, sizeof info.orm_texture);
                }
                part_name(info, node.name ? node.name : node.mesh->name, "Part", used_parts > 1);
                node_path(info.node_path, sizeof info.node_path, &node, [](const cgltf_node* x) { return x->name; },
                          [](const cgltf_node* x) { return x->parent; });
                info.position = placed.position;
                info.rotation = placed.rotation;
                info.scale = placed.scale;
                parts->push_back(info);
            }
            if (!soup || (part >= 0 && part != this_part)) continue;
            const cgltf_accessor *nrm = nullptr, *uv = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
            }
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
                soup->push_back(make_vertex(wx * scale, -wz * scale, wy * scale, nx, -nz, ny, t[0], t[1]));
            }
        }
    }
    cgltf_free(data);
    return true;
}

} // namespace

namespace {

bool load_source(const char* path, f32 scale, i32 part, Allocator& allocator, Array<rhi::MeshVertex>* soup, Array<MeshPart>* parts, char* error,
                 usize error_capacity) {
    error[0] = '\0';
    if (has_extension(path, ".fbx") || has_extension(path, ".obj")) return load_fbx(path, scale, part, allocator, soup, parts, error, error_capacity);  // ufbx reads OBJ too
    if (has_extension(path, ".gltf") || has_extension(path, ".glb")) return load_gltf(path, scale, part, soup, parts, error, error_capacity);
    std::snprintf(error, error_capacity, "Unsupported file type: %s", path);
    return false;
}

} // namespace

bool list_mesh_parts(const char* path, Array<MeshPart>& out, char* error, usize error_capacity) {
    out.clear();
    return load_source(path, 1.0f, -1, out.allocator(), nullptr, &out, error, error_capacity);
}

bool parse_mesh_key(const char* key, Guid& guid, i32& part) {
    part = -1;
    char text[33];
    std::snprintf(text, sizeof text, "%s", key);
    if (std::strlen(key) > 32) {
        if (key[32] != '#' || !std::isdigit(static_cast<unsigned char>(key[33]))) return false;
        char* end = nullptr;
        const long k = std::strtol(key + 33, &end, 10);
        if (*end || k > 0xFFFF) return false;
        part = static_cast<i32>(k);
    }
    return parse_guid(text, guid);
}

void compute_normals(void* vertices, usize count, usize stride, usize position_offset, usize normal_offset, u32 normals, f32 smoothing_angle,
                     Allocator& allocator) {
    if (normals == kNormalsImport || count < 3) return;
    u8* base = static_cast<u8*>(vertices);
    auto position = [&](usize i) { return reinterpret_cast<const f32*>(base + i * stride + position_offset); };
    const usize faces = count / 3;
    Array<Vec3> face(allocator);  // area-weighted (unnormalised) face normals
    face.resize(faces);
    for (usize f = 0; f < faces; ++f) {
        const f32* a = position(f * 3);
        const f32* b = position(f * 3 + 1);
        const f32* c = position(f * 3 + 2);
        face[f] = cross(Vec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, Vec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]});
    }
    auto store = [&](usize i, Vec3 n) {
        const f32 len = length(n);
        n = len > 0.0f ? n * (1.0f / len) : Vec3{0, 0, 1};
        f32* out = reinterpret_cast<f32*>(base + i * stride + normal_offset);
        out[0] = n.x;
        out[1] = n.y;
        out[2] = n.z;
    };
    if (normals == kNormalsFlat) {
        for (usize i = 0; i < faces * 3; ++i) store(i, face[i / 3]);
        return;
    }
    // Corners grouped by position (counting sort on the position remap).
    Array<u32> remap(allocator), start(allocator), order(allocator);
    remap.resize(count);
    meshopt_generatePositionRemap(remap.data(), position(0), count, stride);
    start.resize(count + 1);
    for (usize i = 0; i <= count; ++i) start[i] = 0;
    for (usize i = 0; i < faces * 3; ++i) ++start[remap[i] + 1];
    for (usize i = 0; i < count; ++i) start[i + 1] += start[i];
    order.resize(faces * 3);
    Array<u32> fill(allocator);
    fill.resize(count);
    for (usize i = 0; i < count; ++i) fill[i] = start[i];
    for (usize i = 0; i < faces * 3; ++i) order[fill[remap[i]]++] = static_cast<u32>(i);
    const bool by_angle = normals == kNormalsAutoSmooth;
    const f32 min_cos = std::cos(smoothing_angle * 0.017453293f);
    for (usize i = 0; i < faces * 3; ++i) {
        const Vec3 own = face[i / 3];
        const f32 own_len = length(own);
        Vec3 sum{};
        for (u32 k = start[remap[i]]; k < start[remap[i] + 1]; ++k) {
            const Vec3 other = face[order[k] / 3];
            if (by_angle) {  // only faces within the angle of this one: hard edges stay hard
                const f32 other_len = length(other);
                if (own_len > 0.0f && other_len > 0.0f && dot(own, other) < min_cos * own_len * other_len) continue;
            }
            sum = sum + other;
        }
        store(i, sum);
    }
}

bool import_mesh(const char* path, f32 scale, Allocator& allocator, render::MeshData& out, char* error, usize error_capacity, i32 part, u32 normals,
                 f32 smoothing_angle) {
    Array<rhi::MeshVertex> soup(allocator);  // unindexed triangle list
    if (!load_source(path, scale, part, allocator, &soup, nullptr, error, error_capacity)) return false;
    if (soup.empty()) {
        if (part >= 0) std::snprintf(error, error_capacity, "No part %d in %s", part, path);
        else std::snprintf(error, error_capacity, "No triangles found in %s", path);
        return false;
    }
    compute_normals(soup.data(), soup.size(), normals, smoothing_angle, allocator);

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

u32 simplify_mesh(const render::MeshData& mesh, f32 ratio, u32* out) {
    const usize target = static_cast<usize>(static_cast<f32>(mesh.index_count) * ratio) / 3 * 3;
    constexpr f32 kMaxError = 0.05f;  // of the mesh's size: a distant LOD may drift this far
    return static_cast<u32>(meshopt_simplify(out, mesh.indices, mesh.index_count, mesh.vertices[0].position, mesh.vertex_count,
                                             sizeof(rhi::MeshVertex), target, kMaxError, 0, nullptr));
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
                        char* compiled_path, usize compiled_path_capacity, i32 part) {
    char guid[33];
    to_string(record.guid, guid);
    if (part >= 0) std::snprintf(compiled_path, compiled_path_capacity, "%s/%s_%d.lsdmesh", data_dir, guid, part);
    else std::snprintf(compiled_path, compiled_path_capacity, "%s/%s.lsdmesh", data_dir, guid);

    AssetMeta meta;
    registry.load_meta(record, meta);
    CompiledMeshSource source{record.source_size, record.source_time, 0};
    char settings[96];
    std::snprintf(settings, sizeof settings, "scale=%.6g part=%d local normals=%u/%.3g", static_cast<f64>(meta.import_scale), part, meta.normals,
                  static_cast<f64>(meta.smoothing_angle));  // "local": parts in node space
    source.settings_hash = static_cast<u32>(hash64(settings));
    if (compiled_mesh_up_to_date(compiled_path, source)) return true;

    char path[300], error[256];
    registry.source_path(record, path, sizeof path);
    render::MeshData mesh;
    if (!import_mesh(path, meta.import_scale, allocator, mesh, error, sizeof error, part, meta.normals, meta.smoothing_angle)) {
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

bool compile_mesh_key(const AssetRegistry& registry, const char* key, const char* data_dir, Allocator& allocator, char* compiled_path,
                      usize compiled_path_capacity) {
    Guid guid;
    i32 part = -1;
    if (!parse_mesh_key(key, guid, part)) return false;
    const AssetRecord* record = registry.find(guid);
    return record && record->type == AssetType::Mesh &&
           compile_mesh_asset(registry, *record, data_dir, allocator, compiled_path, compiled_path_capacity, part);
}

} // namespace lsd::asset
