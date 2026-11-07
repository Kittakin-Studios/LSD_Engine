#include "lsd/animation/animation.h"

#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/log.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/ik_aim_job.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/maths/soa_transform.h>
#include <cgltf.h>
#include <ufbx.h>

#include <algorithm>
#include <cctype>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsd::anim {

struct SkinnedModel::Impl {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
};

struct Clip::Impl {
    ozz::unique_ptr<ozz::animation::Animation> animation;
    struct MorphCurve {
        std::string name;
        std::vector<f32> weights;  // 30 a second, 0..1
    };
    std::vector<MorphCurve> morphs;
};

namespace {

Mat4 to_mat4(const ufbx_matrix& m) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 3; ++row) r.at(row, c) = static_cast<f32>(m.cols[c].v[row]);
    }
    return r;
}

ozz::math::Transform to_ozz(const ufbx_transform& t, f32 scale = 1.0f) {
    ozz::math::Transform o;
    o.translation = ozz::math::Float3(static_cast<f32>(t.translation.x) * scale, static_cast<f32>(t.translation.y) * scale,
                                      static_cast<f32>(t.translation.z) * scale);
    o.rotation = ozz::math::Quaternion(static_cast<f32>(t.rotation.x), static_cast<f32>(t.rotation.y), static_cast<f32>(t.rotation.z),
                                       static_cast<f32>(t.rotation.w));
    o.scale = ozz::math::Float3(static_cast<f32>(t.scale.x) * scale, static_cast<f32>(t.scale.y) * scale, static_cast<f32>(t.scale.z) * scale);
    return o;
}

ufbx_scene* load_scene(const char* path, char* error, usize capacity) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    ufbx_error ufbx_err{};
    ufbx_scene* scene = ufbx_load_file(path, &opts, &ufbx_err);
    if (!scene && error) std::snprintf(error, capacity, "FBX: %s", ufbx_err.description.data);
    return scene;
}

// Adds `node` and the needed part of its subtree; returns false if nothing below it is needed.
bool add_joint(const ufbx_node* node, const std::vector<bool>& needed, f32 root_scale, ozz::animation::offline::RawSkeleton::Joint& out) {
    out.name = node->is_root ? "Root" : node->name.data;
    out.transform = to_ozz(node->local_transform);
    if (node->is_root) out.transform.scale = out.transform.scale * root_scale;  // the asset's import scale, around the origin
    for (usize i = 0; i < node->children.count; ++i) {
        const ufbx_node* child = node->children.data[i];
        if (!needed[child->typed_id]) continue;
        ozz::animation::offline::RawSkeleton::Joint joint;
        add_joint(child, needed, root_scale, joint);
        out.children.push_back(std::move(joint));
    }
    return true;
}

} // namespace

SkinnedModel::~SkinnedModel() { delete impl; }
u32 SkinnedModel::joint_count() const { return impl && impl->skeleton ? static_cast<u32>(impl->skeleton->num_joints()) : 0; }
const char* SkinnedModel::joint_name(u32 joint) const { return impl->skeleton->joint_names()[joint]; }
i32 SkinnedModel::joint_parent(u32 joint) const { return impl->skeleton->joint_parents()[joint]; }

Rig detect_rig(const char* const* names, u32 count) {
    auto has = [&](const char* bone) {
        for (u32 i = 0; i < count; ++i) {
            if (names[i] && std::strcmp(names[i], bone) == 0) return true;
        }
        return false;
    };
    if (!has("pelvis") || !has("spine_01") || !has("thigh_l") || !has("head") || !has("upperarm_l")) return Rig::Other;
    if (has("spine_05")) return has("upperarm_twist_01_l") || has("lowerarm_twist_01_l") ? Rig::UE5Manny : Rig::UE5MannySimple;
    if (has("spine_03")) return Rig::UE4Mannequin;
    return Rig::Other;
}

const char* rig_name(Rig rig) {
    switch (rig) {
        case Rig::UE4Mannequin: return "UE4 Mannequin";
        case Rig::UE5Manny: return "UE5 Manny";
        case Rig::UE5MannySimple: return "UE5 Manny (Simple)";
        default: return "Other";
    }
}

const char* rig_source_bone(Rig target, Rig source, const char* bone) {
    const bool ue5_target = target == Rig::UE5Manny || target == Rig::UE5MannySimple;
    const bool ue5_source = source == Rig::UE5Manny || source == Rig::UE5MannySimple;
    struct Pair {
        const char* target;
        const char* source;
    };
    // UE4's three spine bones spread over UE5's five (and back); the neck likewise.
    static const Pair kFromUe4[] = {{"spine_02", "spine_02"}, {"spine_03", "spine_02"}, {"spine_04", "spine_03"}, {"spine_05", "spine_03"}, {"neck_02", "neck_01"}};
    static const Pair kFromUe5[] = {{"spine_02", "spine_03"}, {"spine_03", "spine_05"}, {"neck_01", "neck_02"}};
    const Pair* pairs = nullptr;
    usize n = 0;
    if (ue5_target && source == Rig::UE4Mannequin) {
        pairs = kFromUe4;
        n = sizeof kFromUe4 / sizeof kFromUe4[0];
    } else if (target == Rig::UE4Mannequin && ue5_source) {
        pairs = kFromUe5;
        n = sizeof kFromUe5 / sizeof kFromUe5[0];
    }
    for (usize i = 0; i < n; ++i) {
        if (std::strcmp(pairs[i].target, bone) == 0) return pairs[i].source;
    }
    return bone;
}

Rig model_rig(const SkinnedModel& model) {
    std::vector<const char*> names(model.joint_count());
    for (u32 j = 0; j < model.joint_count(); ++j) names[j] = model.joint_name(j);
    return detect_rig(names.data(), static_cast<u32>(names.size()));
}

i32 SkinnedModel::find_joint(const char* name) const {
    if (!impl || !impl->skeleton || !name) return -1;
    const auto names = impl->skeleton->joint_names();
    for (usize j = 0; j < names.size(); ++j) {
        if (std::strcmp(names[j], name) == 0) return static_cast<i32>(j);
    }
    return -1;
}

namespace {

// The rest pose per joint, unpacked from ozz's SoA layout.
std::vector<ozz::math::Transform> rest_poses(const ozz::animation::Skeleton& skeleton) {
    const u32 joints = static_cast<u32>(skeleton.num_joints());
    std::vector<ozz::math::Transform> rest(joints);
    std::vector<ozz::math::SoaTransform> soa(skeleton.joint_rest_poses().begin(), skeleton.joint_rest_poses().end());
    for (u32 j = 0; j < joints; ++j) {
        const ozz::math::SoaTransform& s = soa[j / 4];
        const int lane = static_cast<int>(j % 4);
        float tx[4], ty[4], tz[4], rx[4], ry[4], rz[4], rw[4], sx[4], sy[4], sz[4];
        ozz::math::StorePtrU(s.translation.x, tx);
        ozz::math::StorePtrU(s.translation.y, ty);
        ozz::math::StorePtrU(s.translation.z, tz);
        ozz::math::StorePtrU(s.rotation.x, rx);
        ozz::math::StorePtrU(s.rotation.y, ry);
        ozz::math::StorePtrU(s.rotation.z, rz);
        ozz::math::StorePtrU(s.rotation.w, rw);
        ozz::math::StorePtrU(s.scale.x, sx);
        ozz::math::StorePtrU(s.scale.y, sy);
        ozz::math::StorePtrU(s.scale.z, sz);
        rest[j].translation = ozz::math::Float3(tx[lane], ty[lane], tz[lane]);
        rest[j].rotation = ozz::math::Quaternion(rx[lane], ry[lane], rz[lane], rw[lane]);
        rest[j].scale = ozz::math::Float3(sx[lane], sy[lane], sz[lane]);
    }
    return rest;
}

// Triangles grouped by section (counting sort, file order kept inside each), so each section is one range.
void group_sections(SkinnedModel& out, const std::vector<u32>& triangle_section) {
    std::vector<u32> grouped(out.indices.size());
    u32 at = 0;
    for (u32 s = 0; s < out.sections.size(); ++s) {
        out.sections[s].first_index = at;
        for (usize t = 0; t < triangle_section.size(); ++t) {
            if (triangle_section[t] != s) continue;
            for (int c = 0; c < 3; ++c) grouped[at++] = out.indices[t * 3 + static_cast<usize>(c)];
        }
        out.sections[s].index_count = at - out.sections[s].first_index;
    }
    if (!grouped.empty()) std::memcpy(out.indices.data(), grouped.data(), grouped.size() * sizeof(u32));
}

// Up to 4 influences renormalised to bytes summing to exactly 255; none = the first joint (the root).
void pack_weights(rhi::SkinnedVertex& v, const u32* joints, const f32* weights_in) {
    f32 weights[4] = {weights_in[0], weights_in[1], weights_in[2], weights_in[3]};
    f32 total = weights[0] + weights[1] + weights[2] + weights[3];
    if (total <= 0.0f) {
        weights[0] = 1.0f;
        total = 1.0f;
    }
    u32 sum = 0;
    for (int w = 0; w < 4; ++w) {
        v.joints[w] = static_cast<u8>(joints[w]);
        v.weights[w] = static_cast<u8>(weights[w] / total * 255.0f + 0.5f);
        sum += v.weights[w];
    }
    v.weights[0] = static_cast<u8>(v.weights[0] + 255 - static_cast<i32>(sum));
}

// Moves `begin` / `length` inside the take when clip `clip` has a trim; shorter than a frame is ignored.
void apply_trim(const ClipTrim* trims, u32 count, u32 clip, f64& begin, f64& length) {
    for (u32 i = 0; i < count; ++i) {
        if (trims[i].clip != clip) continue;
        const f64 s = std::fmin(std::fmax(static_cast<f64>(trims[i].start), 0.0), length);
        const f64 e = trims[i].end > 0.0f ? std::fmin(static_cast<f64>(trims[i].end), length) : length;
        if (e - s < 1.0 / 30.0) return;
        begin += s;
        length = e - s;
        return;
    }
}

bool is_gltf(const char* path) {
    auto ends = [&](const char* ext) {
        const usize n = std::strlen(path), e = std::strlen(ext);
        if (n < e) return false;
        for (usize i = 0; i < e; ++i) {
            if (std::tolower(static_cast<unsigned char>(path[n - e + i])) != ext[i]) return false;
        }
        return true;
    };
    return ends(".gltf") || ends(".glb");
}

cgltf_data* load_gltf(const char* path, char* error, usize capacity) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        if (error) std::snprintf(error, capacity, "glTF: could not read %s", path);
        cgltf_free(data);
        return nullptr;
    }
    return data;
}

// A node's joint name: its own, or node_<index> when it has none or shares it (joints are matched by name).
std::string gltf_joint_name(const cgltf_data* data, const cgltf_node* node) {
    const cgltf_size index = cgltf_node_index(data, node);
    if (node->name && node->name[0] && std::strcmp(node->name, "Root") != 0) {
        bool shared = false;
        for (cgltf_size i = 0; i < data->nodes_count && !shared; ++i) shared = i != index && data->nodes[i].name && std::strcmp(data->nodes[i].name, node->name) == 0;
        if (!shared) return node->name;
    }
    return "node_" + std::to_string(index);
}

ozz::math::Transform gltf_local(const cgltf_node* n) {
    ozz::math::Transform t = ozz::math::Transform::identity();
    if (n->has_matrix) {
        ozz::math::Float4x4 m;
        for (int c = 0; c < 4; ++c) m.cols[c] = ozz::math::simd_float4::LoadPtrU(n->matrix + c * 4);
        if (!ozz::math::ToAffine(m, &t)) t = ozz::math::Transform::identity();
        return t;
    }
    if (n->has_translation) t.translation = ozz::math::Float3(n->translation[0], n->translation[1], n->translation[2]);
    if (n->has_rotation) t.rotation = ozz::math::Quaternion(n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]);
    if (n->has_scale) t.scale = ozz::math::Float3(n->scale[0], n->scale[1], n->scale[2]);
    return t;
}

void add_gltf_joint(const cgltf_data* data, const cgltf_node* node, const std::vector<bool>& needed, ozz::animation::offline::RawSkeleton::Joint& out) {
    out.name = gltf_joint_name(data, node).c_str();
    out.transform = gltf_local(node);
    for (cgltf_size i = 0; i < node->children_count; ++i) {
        const cgltf_node* child = node->children[i];
        if (!needed[cgltf_node_index(data, child)]) continue;
        ozz::animation::offline::RawSkeleton::Joint joint;
        add_gltf_joint(data, child, needed, joint);
        out.children.push_back(std::move(joint));
    }
}

// glTF is Y-up: C turns a file point (x, y, z) into the engine's (x, -z, y), as for static meshes.
Mat4 gltf_up() {
    Mat4 c;
    c.at(1, 1) = 0, c.at(1, 2) = -1, c.at(2, 1) = 1, c.at(2, 2) = 0;
    return c;
}

// The first skin's meshes, with a "Root" joint carrying the Y-up turn and the import scale (FBX's scene root does
// the same). Vertices and bind matrices are in engine axes, unscaled, like the FBX path.
// shortcut: one skin, no morph targets from glTF yet; add them when a rigged glTF with several skins or shape keys turns up.
bool import_skinned_gltf(const char* path, f32 scale, SkinnedModel& out, char* error, usize error_capacity, u32 normals, f32 smoothing_angle) {
    cgltf_data* data = load_gltf(path, error, error_capacity);
    if (!data) return false;
    const cgltf_skin* skin = data->skins_count ? &data->skins[0] : nullptr;
    if (!skin || !skin->joints_count) {
        std::snprintf(error, error_capacity, "No skinned mesh (no skeleton) in %s", path);
        cgltf_free(data);
        return false;
    }
    std::vector<bool> needed(data->nodes_count, false);
    for (cgltf_size i = 0; i < skin->joints_count; ++i) {
        for (const cgltf_node* n = skin->joints[i]; n; n = n->parent) needed[cgltf_node_index(data, n)] = true;
    }
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    ozz::animation::offline::RawSkeleton::Joint& root = raw.roots[0];
    root.name = "Root";
    root.transform = ozz::math::Transform::identity();
    root.transform.rotation = ozz::math::Quaternion(0.70710678f, 0.0f, 0.0f, 0.70710678f);  // +90 degrees about X: Y up becomes Z up
    root.transform.scale = ozz::math::Float3(scale, scale, scale);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (!needed[i] || data->nodes[i].parent) continue;
        ozz::animation::offline::RawSkeleton::Joint joint;
        add_gltf_joint(data, &data->nodes[i], needed, joint);
        root.children.push_back(std::move(joint));
    }
    delete out.impl;
    out.impl = new SkinnedModel::Impl();
    out.impl->skeleton = ozz::animation::offline::SkeletonBuilder()(raw);
    if (!out.impl->skeleton) {
        std::snprintf(error, error_capacity, "Skeleton could not be built (more than %d joints?)", ozz::animation::Skeleton::kMaxJoints);
        cgltf_free(data);
        return false;
    }
    const u32 joints = out.joint_count();
    std::vector<i32> joint_of(data->nodes_count, -1);  // node -> ozz joint (ozz orders joints its own way)
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (needed[i]) joint_of[i] = out.find_joint(gltf_joint_name(data, &data->nodes[i]).c_str());
    }
    const Mat4 up = gltf_up(), down = affine_inverse(up);
    // Inverse binds: the skin's for its joints, the rest pose for the others; the Root's is the turn itself.
    out.inverse_bind.clear();
    out.inverse_bind.resize(joints);
    for (u32 j = 0; j < joints; ++j) out.inverse_bind[j] = Mat4{};
    out.inverse_bind[static_cast<u32>(out.find_joint("Root"))] = down;
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (joint_of[i] < 0) continue;
        Mat4 world;
        cgltf_node_transform_world(&data->nodes[i], world.m);
        out.inverse_bind[static_cast<u32>(joint_of[i])] = affine_inverse(world) * down;
    }
    if (skin->inverse_bind_matrices) {
        for (cgltf_size k = 0; k < skin->joints_count; ++k) {
            const i32 j = joint_of[cgltf_node_index(data, skin->joints[k])];
            Mat4 ibm;
            if (j >= 0 && cgltf_accessor_read_float(skin->inverse_bind_matrices, k, ibm.m, 16)) out.inverse_bind[static_cast<u32>(j)] = ibm * down;
        }
    }
    out.vertices.clear();
    out.indices.clear();
    out.sections.clear();
    out.morphs.clear();
    out.morph_deltas.clear();
    out.bounds_min = {1e30f, 1e30f, 1e30f};
    out.bounds_max = {-1e30f, -1e30f, -1e30f};
    std::vector<u32> triangle_section;
    auto section_of = [&](const char* material) {
        for (u32 s = 0; s < out.sections.size(); ++s) {
            if (std::strcmp(out.sections[s].material, material) == 0) return s;
        }
        MeshSection section;
        std::snprintf(section.material, sizeof section.material, "%s", material);
        out.sections.push_back(section);
        return static_cast<u32>(out.sections.size() - 1);
    };
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        if (node.skin != skin) {
            LSD_LOG_WARN("anim", "%s: mesh '%s' is not on the first skin and is left out", path, node.mesh->name ? node.mesh->name : "");
            continue;
        }
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
                else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) jnt = attr.data;
                else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) wgt = attr.data;
            }
            if (!pos) continue;
            const u32 section = section_of(prim.material && prim.material->name ? prim.material->name : "");
            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            for (cgltf_size t = 0; t + 3 <= count; t += 3) triangle_section.push_back(section);
            for (cgltf_size i = 0; i < count - count % 3; ++i) {
                const cgltf_size v = prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i;
                cgltf_float lp[3] = {0, 0, 0}, ln[3] = {0, 1, 0}, tc[2] = {0, 0}, w[4] = {0, 0, 0, 0};
                cgltf_uint ji[4] = {0, 0, 0, 0};
                cgltf_accessor_read_float(pos, v, lp, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, ln, 3);
                if (uv) cgltf_accessor_read_float(uv, v, tc, 2);  // glTF UVs already start at the top, like textures
                if (jnt && wgt) {
                    cgltf_accessor_read_uint(jnt, v, ji, 4);
                    cgltf_accessor_read_float(wgt, v, w, 4);
                }
                u32 joint_ids[4] = {};
                for (int k = 0; k < 4; ++k) {
                    const i32 j = ji[k] < skin->joints_count ? joint_of[cgltf_node_index(data, skin->joints[ji[k]])] : -1;
                    if (j < 0) w[k] = 0.0f;
                    else joint_ids[k] = static_cast<u32>(j);
                }
                rhi::SkinnedVertex out_v{};
                const Vec3 p3 = transform_point(up, {lp[0], lp[1], lp[2]});
                Vec3 n3 = transform_vector(up, {ln[0], ln[1], ln[2]});
                const f32 len = length(n3);
                n3 = len > 0.0f ? n3 * (1.0f / len) : Vec3{0, 0, 1};
                out_v.position[0] = p3.x, out_v.position[1] = p3.y, out_v.position[2] = p3.z;
                out_v.normal[0] = n3.x, out_v.normal[1] = n3.y, out_v.normal[2] = n3.z;
                out_v.uv[0] = tc[0], out_v.uv[1] = tc[1];
                pack_weights(out_v, joint_ids, w);
                out.indices.push_back(static_cast<u32>(out.vertices.size()));  // shortcut: no welding for skinned meshes yet
                out.vertices.push_back(out_v);
                out.bounds_min = vmin(out.bounds_min, p3 * scale);  // bind pose (glTF binds where the mesh rests)
                out.bounds_max = vmax(out.bounds_max, p3 * scale);
            }
        }
    }
    if (out.bounds_min.x > out.bounds_max.x) out.bounds_min = out.bounds_max = {};
    group_sections(out, triangle_section);
    asset::compute_normals(out.vertices.data(), out.vertices.size(), sizeof(rhi::SkinnedVertex), offsetof(rhi::SkinnedVertex, position),
                           offsetof(rhi::SkinnedVertex, normal), normals, smoothing_angle, out.vertices.allocator());
    if (out.vertices.size() > 0 && joints > 255) LSD_LOG_WARN("anim", "%s: %u joints; vertices can only use the first 256", path, joints);
    cgltf_free(data);
    return !out.vertices.empty();
}

// One glTF animation channel's keys, read once.
struct GltfTrack {
    const cgltf_animation_channel* channel = nullptr;
    std::vector<f32> times, values;  // values: comps per key (x3 for cubic spline: in-tangent, value, out-tangent)
    u32 comps = 3;
    // The value at `t` (linear; step holds; cubic spline uses its key values linearly).
    void sample(f32 t, f32* out) const {
        const usize keys = times.size();
        const usize stride = channel->sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1;
        const usize offset = stride == 3 ? 1 : 0;
        auto value = [&](usize k) { return &values[(k * stride + offset) * comps]; };
        if (keys == 0) return;
        usize k = static_cast<usize>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
        if (k == 0 || k >= keys) {
            const f32* v = value(k == 0 ? 0 : keys - 1);
            for (u32 c = 0; c < comps; ++c) out[c] = v[c];
            return;
        }
        const f32* a = value(k - 1);
        const f32* b = value(k);
        f32 f = channel->sampler->interpolation == cgltf_interpolation_type_step ? 0.0f : (t - times[k - 1]) / (times[k] - times[k - 1]);
        f32 sign = 1.0f;
        if (comps == 4 && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) sign = -1.0f;  // the short way round
        f32 len = 0.0f;
        for (u32 c = 0; c < comps; ++c) {
            out[c] = a[c] * (1.0f - f) + b[c] * sign * f;
            len += out[c] * out[c];
        }
        if (comps == 4 && len > 0.0f) {  // shortcut: normalised lerp, not slerp; fine at 30 fps keys
            const f32 inv = 1.0f / std::sqrt(len);
            for (u32 c = 0; c < 4; ++c) out[c] *= inv;
        }
    }
};

// Each glTF animation becomes a clip, bones matched by name (so a glTF of takes drives a model from another file).
// shortcut: no retargeting between rigs and no morph weights from glTF yet; the FBX path does both.
u32 import_clips_gltf(const char* path, const SkinnedModel& model, Clip* out, u32 capacity, const ClipTrim* trims, u32 trim_count) {
    char error[256];
    cgltf_data* data = load_gltf(path, error, sizeof error);
    if (!data || !model.impl) {
        if (!data) LSD_LOG_WARN("anim", "%s", error);
        cgltf_free(data);
        return 0;
    }
    const auto& skeleton = *model.impl->skeleton;
    const u32 joints = static_cast<u32>(skeleton.num_joints());
    const std::vector<ozz::math::Transform> rest = rest_poses(skeleton);
    std::vector<i32> joint_of(data->nodes_count, -1);
    std::vector<const cgltf_node*> nodes(joints, nullptr);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const i32 j = model.find_joint(gltf_joint_name(data, &data->nodes[i]).c_str());  // never "Root": the model keeps its own
        if (j < 0 || nodes[static_cast<u32>(j)]) continue;
        joint_of[i] = j;
        nodes[static_cast<u32>(j)] = &data->nodes[i];
    }
    u32 written = 0;
    for (cgltf_size a = 0; a < data->animations_count && written < capacity; ++a) {
        const cgltf_animation& anim = data->animations[a];
        std::vector<std::vector<GltfTrack>> tracks(joints);
        f32 begin = 1e30f, end = -1e30f;
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            if (!ch.target_node || !ch.sampler || ch.target_path == cgltf_animation_path_type_weights || ch.target_path == cgltf_animation_path_type_invalid) continue;
            const i32 j = joint_of[cgltf_node_index(data, ch.target_node)];
            if (j < 0) continue;
            GltfTrack track;
            track.channel = &ch;
            track.comps = ch.target_path == cgltf_animation_path_type_rotation ? 4 : 3;
            track.times.resize(ch.sampler->input->count);
            for (cgltf_size k = 0; k < track.times.size(); ++k) cgltf_accessor_read_float(ch.sampler->input, k, &track.times[k], 1);
            track.values.resize(ch.sampler->output->count * track.comps);
            for (cgltf_size k = 0; k < ch.sampler->output->count; ++k) cgltf_accessor_read_float(ch.sampler->output, k, &track.values[k * track.comps], track.comps);
            if (track.times.empty() || track.values.size() < track.times.size() * track.comps) continue;
            begin = std::fmin(begin, track.times.front());
            end = std::fmax(end, track.times.back());
            tracks[static_cast<u32>(j)].push_back(std::move(track));
        }
        if (!(end > begin)) continue;
        f64 begin64 = begin, length64 = end - begin;
        apply_trim(trims, trim_count, written, begin64, length64);
        begin = static_cast<f32>(begin64);
        const f32 length = static_cast<f32>(length64);
        ozz::animation::offline::RawAnimation raw;
        raw.duration = length;
        raw.tracks.resize(joints);
        const u32 frames = static_cast<u32>(std::ceil(length * 30.0f)) + 1;
        for (u32 j = 0; j < joints; ++j) {
            auto& track = raw.tracks[j];
            const ozz::math::Transform base = nodes[j] ? gltf_local(nodes[j]) : rest[j];
            ozz::math::Quaternion previous = base.rotation;
            for (u32 f = 0; f < frames; ++f) {
                const f32 t = f + 1 == frames ? length : static_cast<f32>(f) / 30.0f;
                ozz::math::Transform local = base;
                for (const GltfTrack& g : tracks[j]) {
                    f32 v[4];
                    g.sample(begin + t, v);
                    if (g.channel->target_path == cgltf_animation_path_type_translation) local.translation = ozz::math::Float3(v[0], v[1], v[2]);
                    else if (g.channel->target_path == cgltf_animation_path_type_rotation) local.rotation = ozz::math::Quaternion(v[0], v[1], v[2], v[3]);
                    else local.scale = ozz::math::Float3(v[0], v[1], v[2]);
                }
                const f32 d = local.rotation.x * previous.x + local.rotation.y * previous.y + local.rotation.z * previous.z + local.rotation.w * previous.w;
                if (d < 0.0f) local.rotation = ozz::math::Quaternion(-local.rotation.x, -local.rotation.y, -local.rotation.z, -local.rotation.w);
                previous = local.rotation;
                track.translations.push_back({t, local.translation});
                track.rotations.push_back({t, local.rotation});
                track.scales.push_back({t, local.scale});
            }
        }
        Clip& clip = out[written];
        delete clip.impl;
        clip.impl = new Clip::Impl();
        clip.impl->animation = ozz::animation::offline::AnimationBuilder()(raw);
        if (!clip.impl->animation) {
            LSD_LOG_WARN("anim", "%s: animation '%s' could not be built", path, anim.name ? anim.name : "");
            continue;
        }
        std::snprintf(clip.name, sizeof clip.name, "%s", anim.name && anim.name[0] ? anim.name : "Take");
        clip.duration = raw.duration;
        ++written;
    }
    cgltf_free(data);
    return written;
}

} // namespace

bool import_skinned(const char* path, f32 scale, SkinnedModel& out, char* error, usize error_capacity, u32 normals, f32 smoothing_angle) {
    if (is_gltf(path)) return import_skinned_gltf(path, scale, out, error, error_capacity, normals, smoothing_angle);
    ufbx_scene* scene = load_scene(path, error, error_capacity);
    if (!scene) return false;
    // Joints: every bone a skin uses, and their parents up to the scene root (which carries the
    // axis / unit conversion, so ozz's model space is the engine's).
    std::vector<bool> needed(scene->nodes.count, false);
    const ufbx_mesh* skinned_mesh = nullptr;
    for (usize m = 0; m < scene->meshes.count; ++m) {
        const ufbx_mesh* mesh = scene->meshes.data[m];
        if (!mesh->skin_deformers.count) continue;
        skinned_mesh = skinned_mesh ? skinned_mesh : mesh;
        for (const ufbx_skin_cluster* cluster : mesh->skin_deformers.data[0]->clusters) {
            for (const ufbx_node* n = cluster->bone_node; n; n = n->parent) needed[n->typed_id] = true;
        }
    }
    if (!skinned_mesh) {
        std::snprintf(error, error_capacity, "No skinned mesh (no skeleton) in %s", path);
        ufbx_free_scene(scene);
        return false;
    }
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    add_joint(scene->root_node, needed, scale, raw.roots[0]);
    delete out.impl;
    out.impl = new SkinnedModel::Impl();
    out.impl->skeleton = ozz::animation::offline::SkeletonBuilder()(raw);
    if (!out.impl->skeleton) {
        std::snprintf(error, error_capacity, "Skeleton could not be built (more than %d joints?)", ozz::animation::Skeleton::kMaxJoints);
        ufbx_free_scene(scene);
        return false;
    }
    const auto& skeleton = *out.impl->skeleton;
    const u32 joints = static_cast<u32>(skeleton.num_joints());
    auto joint_of = [&](const ufbx_node* node) -> i32 {
        const char* name = node->is_root ? "Root" : node->name.data;
        for (u32 j = 0; j < joints; ++j) {
            if (std::strcmp(skeleton.joint_names()[j], name) == 0) return static_cast<i32>(j);
        }
        return -1;
    };
    // Inverse bind per joint: the first skin's geometry_to_bone (bone_world(t) x geometry_to_bone
    // skins that mesh's vertices). Other skinned meshes are first moved into that mesh's space.
    out.inverse_bind.clear();
    out.inverse_bind.resize(joints);  // Array does not construct: start every joint at identity
    for (u32 j = 0; j < joints; ++j) out.inverse_bind[j] = Mat4{};
    std::vector<bool> has_bind(joints, false);
    const ufbx_skin_deformer* main_skin = skinned_mesh->skin_deformers.data[0];
    for (const ufbx_skin_cluster* c : main_skin->clusters) {
        const i32 j = c->bone_node ? joint_of(c->bone_node) : -1;
        if (j < 0 || has_bind[j]) continue;
        out.inverse_bind[static_cast<u32>(j)] = to_mat4(c->geometry_to_bone);
        has_bind[j] = true;
    }
    // Scale on the root scales the skinned result; the bind matrices are in unscaled space.
    out.vertices.clear();
    out.indices.clear();
    out.sections.clear();
    out.morphs.clear();
    out.morph_deltas.clear();
    std::vector<u32> triangle_section;  // per triangle written, its section (indices are grouped by it at the end)
    auto section_of = [&](const char* material) {
        for (u32 s = 0; s < out.sections.size(); ++s) {
            if (std::strcmp(out.sections[s].material, material) == 0) return s;
        }
        MeshSection section;
        std::snprintf(section.material, sizeof section.material, "%s", material);
        out.sections.push_back(section);
        return static_cast<u32>(out.sections.size() - 1);
    };
    std::vector<u32> tri;
    std::vector<u32> corner_vertex;  // this mesh's output vertices -> its ufbx vertex (morph offsets are per vertex)
    out.bounds_min = {1e30f, 1e30f, 1e30f};
    out.bounds_max = {-1e30f, -1e30f, -1e30f};
    for (usize m = 0; m < scene->meshes.count; ++m) {
        const ufbx_mesh* mesh = scene->meshes.data[m];
        if (!mesh->skin_deformers.count) {
            LSD_LOG_WARN("anim", "%s: mesh '%s' has no skin and is left out (shortcut: parent it to a bone in the DCC)", path, mesh->name.data);
            continue;
        }
        const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
        // This mesh into the main mesh's space, through a bone both know (none needed for the main mesh).
        Mat4 into_main;
        if (mesh != skinned_mesh) {
            for (const ufbx_skin_cluster* c : skin->clusters) {
                const i32 j = c->bone_node ? joint_of(c->bone_node) : -1;
                if (j >= 0 && has_bind[j]) {
                    into_main = affine_inverse(out.inverse_bind[static_cast<u32>(j)]) * to_mat4(c->geometry_to_bone);
                    break;
                }
            }
        }
        const Mat4 normal_into_main = [&] {
            Mat4 n = affine_inverse(into_main);
            Mat4 t;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) t.at(r, c) = n.at(c, r);
            return t;
        }();
        // Vertices without weights follow the root (which never animates): placed by the mesh's own
        // transform relative to it, as ufbx does.
        const Mat4 unweighted = mesh->instances.count
            ? affine_inverse(to_mat4(scene->root_node->node_to_world)) * to_mat4(mesh->instances.data[0]->geometry_to_world)
            : Mat4{};
        const Mat4 normal_unweighted = [&] {
            Mat4 n = affine_inverse(unweighted);
            Mat4 t;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) t.at(r, c) = n.at(c, r);
            return t;
        }();
        tri.resize(mesh->max_face_triangles * 3);
        const u32 first_vertex = static_cast<u32>(out.vertices.size());
        corner_vertex.clear();
        // Per-instance materials live on the node (as for static meshes), at the mesh's material indices.
        const ufbx_node* instance = mesh->instances.count ? mesh->instances.data[0] : nullptr;
        for (usize f = 0; f < mesh->faces.count; ++f) {
            const u32 count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[f]);
            const u32 slot = f < mesh->face_material.count ? mesh->face_material.data[f] : 0;
            const ufbx_material* material = instance && slot < instance->materials.count ? instance->materials.data[slot]
                                            : slot < mesh->materials.count                ? mesh->materials.data[slot]
                                                                                           : nullptr;
            const u32 section = section_of(material ? material->name.data : "");
            for (u32 t = 0; t < count; ++t) triangle_section.push_back(section);
            for (u32 i = 0; i < count * 3; ++i) {
                const u32 index = tri[i];
                corner_vertex.push_back(mesh->vertex_indices.data[index]);
                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                const ufbx_vec3 n = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, index) : ufbx_vec3{{{0, 0, 1}}};
                const ufbx_vec2 uv = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2(&mesh->vertex_uv, index) : ufbx_vec2{{{0, 0}}};
                rhi::SkinnedVertex v{};
                const bool weighted = skin->vertices.data[mesh->vertex_indices.data[index]].num_weights > 0;
                const Vec3 pos = transform_point(weighted ? into_main : unweighted, {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)});
                Vec3 nrm = transform_vector(weighted ? normal_into_main : normal_unweighted, {static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)});
                const f32 len = length(nrm);
                nrm = len > 0.0f ? nrm * (1.0f / len) : Vec3{0, 0, 1};
                v.position[0] = pos.x;
                v.position[1] = pos.y;
                v.position[2] = pos.z;
                v.normal[0] = nrm.x;
                v.normal[1] = nrm.y;
                v.normal[2] = nrm.z;
                v.uv[0] = static_cast<f32>(uv.x);
                v.uv[1] = 1.0f - static_cast<f32>(uv.y);  // FBX UVs start at the bottom; textures at the top
                // Up to 4 influences (ufbx sorts them strongest first), renormalised to bytes summing to 255.
                const ufbx_skin_vertex& sv = skin->vertices.data[mesh->vertex_indices.data[index]];
                f32 weights[4] = {};
                u32 joints_of[4] = {};
                f32 total = 0.0f;
                for (u32 w = 0; w < sv.num_weights && w < 4; ++w) {
                    const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + w];
                    const ufbx_skin_cluster* c = skin->clusters.data[sw.cluster_index];
                    const i32 j = c->bone_node ? joint_of(c->bone_node) : -1;
                    if (j < 0) continue;
                    joints_of[w] = static_cast<u32>(j);
                    weights[w] = static_cast<f32>(sw.weight);
                    total += weights[w];
                }
                if (total <= 0.0f) {  // unweighted vertex: follows the root
                    weights[0] = 1.0f;
                    total = 1.0f;
                }
                u32 sum = 0;
                for (int w = 0; w < 4; ++w) {
                    v.joints[w] = static_cast<u8>(joints_of[w]);
                    v.weights[w] = static_cast<u8>(weights[w] / total * 255.0f + 0.5f);
                    sum += v.weights[w];
                }
                v.weights[0] = static_cast<u8>(v.weights[0] + 255 - static_cast<i32>(sum));  // exact 255 total
                out.indices.push_back(static_cast<u32>(out.vertices.size()));  // shortcut: no welding for skinned meshes yet
                out.vertices.push_back(v);
                // Bounds at rest: the strongest bone's current pose.
                const ufbx_skin_cluster* top = sv.num_weights ? skin->clusters.data[skin->weights.data[sv.weight_begin].cluster_index] : nullptr;
                if (top && top->bone_node) {
                    const ufbx_vec3 world = ufbx_transform_position(&top->geometry_to_world, p);
                    const Vec3 wp{static_cast<f32>(world.x) * scale, static_cast<f32>(world.y) * scale, static_cast<f32>(world.z) * scale};
                    out.bounds_min = vmin(out.bounds_min, wp);
                    out.bounds_max = vmax(out.bounds_max, wp);
                }
            }
        }
        // Morph targets: each blend channel's full-weight shape, moved into model space like the vertices.
        for (const ufbx_blend_deformer* blend : mesh->blend_deformers) {
            for (const ufbx_blend_channel* channel : blend->channels) {
                const ufbx_blend_shape* shape = channel->target_shape;
                if (!shape) continue;
                MorphTarget target;
                std::snprintf(target.name, sizeof target.name, "%s", channel->name.data);
                target.first = static_cast<u32>(out.morph_deltas.size());
                for (usize k = 0; k < corner_vertex.size(); ++k) {
                    const ufbx_vec3 o = ufbx_get_blend_shape_vertex_offset(shape, corner_vertex[k]);
                    if (o.x == 0.0 && o.y == 0.0 && o.z == 0.0) continue;
                    const bool weighted = skin->vertices.data[corner_vertex[k]].num_weights > 0;
                    const Vec3 d = transform_vector(weighted ? into_main : unweighted, {static_cast<f32>(o.x), static_cast<f32>(o.y), static_cast<f32>(o.z)});
                    out.morph_deltas.push_back({first_vertex + static_cast<u32>(k), {d.x, d.y, d.z}});
                }
                target.count = static_cast<u32>(out.morph_deltas.size()) - target.first;
                if (target.count) out.morphs.push_back(target);
            }
        }
    }
    if (out.bounds_min.x > out.bounds_max.x) out.bounds_min = out.bounds_max = {};
    group_sections(out, triangle_section);
    asset::compute_normals(out.vertices.data(), out.vertices.size(), sizeof(rhi::SkinnedVertex), offsetof(rhi::SkinnedVertex, position),
                           offsetof(rhi::SkinnedVertex, normal), normals, smoothing_angle, out.vertices.allocator());
    if (out.vertices.size() > 0 && joints > 255) LSD_LOG_WARN("anim", "%s: %u joints; vertices can only use the first 256", path, joints);
    ufbx_free_scene(scene);
    return !out.vertices.empty();
}

Clip::~Clip() { delete impl; }

bool Clip::morph_weight(const char* morph, f32 ratio, f32& weight) const {
    if (!impl) return false;
    for (const Impl::MorphCurve& c : impl->morphs) {
        if (c.name != morph || c.weights.empty()) continue;
        // Samples are 1/30 s apart, except the last, which sits at the very end of the clip.
        const f32 time = (ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio) * duration;
        const usize last = c.weights.size() - 1;
        usize i = static_cast<usize>(time * 30.0f);
        i = i < last ? i : last;
        const f32 t0 = static_cast<f32>(i) / 30.0f;
        const f32 t1 = i + 1 == last ? duration : static_cast<f32>(i + 1) / 30.0f;
        const f32 t = i < last && t1 > t0 ? (time - t0) / (t1 - t0) : 0.0f;
        weight = i < last ? c.weights[i] * (1.0f - t) + c.weights[i + 1] * t : c.weights[last];
        return true;
    }
    return false;
}

bool blended_morph_weight(const BlendInput* inputs, u32 count, const char* morph, f32& weight) {
    f32 sum = 0.0f, total = 0.0f;
    bool any = false;
    for (u32 i = 0; i < count; ++i) {
        f32 w = 0.0f;  // a clip that leaves the morph alone counts as rest (0)
        if (inputs[i].clip && inputs[i].clip->morph_weight(morph, inputs[i].ratio, w)) any = true;
        sum += w * inputs[i].weight;
        total += inputs[i].weight;
    }
    if (any) weight = total > 0.0f ? sum / total : 0.0f;
    return any;
}

u32 import_clips(const char* path, const SkinnedModel& model, Clip* out, u32 capacity, const ClipTrim* trims, u32 trim_count) {
    if (is_gltf(path)) return import_clips_gltf(path, model, out, capacity, trims, trim_count);
    char error[256];
    ufbx_scene* scene = load_scene(path, error, sizeof error);
    if (!scene || !model.impl) {
        if (!scene) LSD_LOG_WARN("anim", "%s", error);
        return 0;
    }
    const auto& skeleton = *model.impl->skeleton;
    const u32 joints = static_cast<u32>(skeleton.num_joints());
    const std::vector<ozz::math::Transform> rest = rest_poses(skeleton);  // for joints this file does not have
    // Which bone of the file drives each joint: the same name, or across UE4 / UE5 rigs its counterpart.
    std::vector<const char*> file_names;
    for (usize i = 0; i < scene->nodes.count; ++i) file_names.push_back(scene->nodes.data[i]->name.data);
    const Rig source_rig = detect_rig(file_names.data(), static_cast<u32>(file_names.size()));
    const Rig target_rig = model_rig(model);
    std::vector<const ufbx_node*> nodes(joints, nullptr);
    for (u32 j = 0; j < joints; ++j) {
        if (std::strcmp(skeleton.joint_names()[j], "Root") == 0) continue;  // keeps the model's conversion and scale
        nodes[j] = ufbx_find_node(scene, rig_source_bone(target_rig, source_rig, skeleton.joint_names()[j]));
    }
    // Retarget when the rigs differ or a driving bone rests differently (another character's
    // animation): copy each bone's world-space change from its rest pose instead of its local values.
    namespace om = ozz::math;
    const auto parents = skeleton.joint_parents();
    std::vector<om::Quaternion> target_rest_world(joints);
    std::vector<om::Float3> target_rest_position(joints), target_rest_scale(joints);
    for (u32 j = 0; j < joints; ++j) {
        const int p = parents[j];
        if (p < 0) {
            target_rest_world[j] = rest[j].rotation;
            target_rest_position[j] = rest[j].translation;
            target_rest_scale[j] = rest[j].scale;
        } else {
            target_rest_world[j] = target_rest_world[p] * rest[j].rotation;
            target_rest_position[j] = target_rest_position[p] + om::TransformVector(target_rest_world[p], target_rest_scale[p] * rest[j].translation);
            target_rest_scale[j] = target_rest_scale[p] * rest[j].scale;
        }
    }
    auto world_rotation = [](const ufbx_node* node, const ufbx_anim* anim, f64 time) {
        om::Quaternion r = om::Quaternion::identity();
        for (const ufbx_node* n = node; n; n = n->parent) r = to_ozz(anim ? ufbx_evaluate_transform(anim, n, time) : n->local_transform).rotation * r;
        return r;
    };
    bool retarget = source_rig != target_rig && source_rig != Rig::Other && target_rig != Rig::Other &&
                    !((source_rig == Rig::UE5Manny || source_rig == Rig::UE5MannySimple) && (target_rig == Rig::UE5Manny || target_rig == Rig::UE5MannySimple));
    std::vector<om::Quaternion> source_rest_world(joints, om::Quaternion::identity());
    for (u32 j = 0; j < joints; ++j) {
        if (!nodes[j]) continue;
        source_rest_world[j] = world_rotation(nodes[j], nullptr, 0.0);
        const om::Quaternion local = to_ozz(nodes[j]->local_transform).rotation;
        const f32 d = std::fabs(local.x * rest[j].rotation.x + local.y * rest[j].rotation.y + local.z * rest[j].rotation.z + local.w * rest[j].rotation.w);
        if (d < 0.996f) retarget = true;  // more than about 10 degrees apart at rest
    }
    // The pelvis moves (walks, crouches) scaled by how much taller the target stands.
    f32 height_ratio = 1.0f;
    i32 pelvis = -1;
    for (u32 j = 0; j < joints && pelvis < 0; ++j) {
        if (std::strcmp(skeleton.joint_names()[j], "pelvis") == 0) pelvis = static_cast<i32>(j);
    }
    if (pelvis >= 0 && nodes[static_cast<u32>(pelvis)]) {
        const f64 source_height = nodes[static_cast<u32>(pelvis)]->node_to_world.m23;
        if (source_height > 1e-3) height_ratio = target_rest_position[static_cast<u32>(pelvis)].z / static_cast<f32>(source_height);
    }
    if (retarget) LSD_LOG_INFO("anim", "%s: retargeting %s animation onto a %s", path, rig_name(source_rig), rig_name(target_rig));
    u32 written = 0;
    for (usize s = 0; s < scene->anim_stacks.count && written < capacity; ++s) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[s];
        f64 begin = stack->time_begin, length = stack->time_end - stack->time_begin;
        if (length <= 0.0) continue;
        apply_trim(trims, trim_count, written, begin, length);
        ozz::animation::offline::RawAnimation raw;
        raw.duration = static_cast<f32>(length);
        raw.tracks.resize(joints);
        const u32 frames = static_cast<u32>(std::ceil(length * 30.0)) + 1;
        std::vector<om::Transform> retargeted;  // frame-major, when retargeting
        if (retarget) {
            retargeted.resize(static_cast<usize>(frames) * joints);
            std::vector<om::Quaternion> world(joints);
            for (u32 f = 0; f < frames; ++f) {
                const f64 t = begin + (f + 1 == frames ? length : static_cast<f64>(f) / 30.0);
                for (u32 j = 0; j < joints; ++j) {
                    const int p = parents[j];
                    const om::Quaternion parent_world = p < 0 ? om::Quaternion::identity() : world[p];
                    om::Transform local = rest[j];
                    if (nodes[j]) {
                        const om::Quaternion change = world_rotation(nodes[j], stack->anim, t) * om::Conjugate(source_rest_world[j]);
                        world[j] = change * target_rest_world[j];
                        local.rotation = om::Conjugate(parent_world) * world[j];
                        if (static_cast<i32>(j) == pelvis || std::strcmp(skeleton.joint_names()[j], "root") == 0) {
                            const om::Float3 moved = to_ozz(ufbx_evaluate_transform(stack->anim, nodes[j], t)).translation - to_ozz(nodes[j]->local_transform).translation;
                            local.translation = rest[j].translation + moved * height_ratio;
                        }
                    } else {
                        world[j] = parent_world * rest[j].rotation;  // no driver (twist and helper bones): stays at rest under its parent
                    }
                    retargeted[static_cast<usize>(f) * joints + j] = local;
                }
            }
        }
        for (u32 j = 0; j < joints; ++j) {
            auto& track = raw.tracks[j];
            ozz::math::Quaternion previous = rest[j].rotation;
            for (u32 f = 0; f < frames; ++f) {
                const f64 t = f + 1 == frames ? length : static_cast<f64>(f) / 30.0;
                ozz::math::Transform local = rest[j];
                if (retarget) local = retargeted[static_cast<usize>(f) * joints + j];
                else if (nodes[j]) local = to_ozz(ufbx_evaluate_transform(stack->anim, nodes[j], begin + t));
                // Keep neighbouring rotations in the same hemisphere so interpolation takes the short way.
                const f32 d = local.rotation.x * previous.x + local.rotation.y * previous.y + local.rotation.z * previous.z + local.rotation.w * previous.w;
                if (d < 0.0f) local.rotation = ozz::math::Quaternion(-local.rotation.x, -local.rotation.y, -local.rotation.z, -local.rotation.w);
                previous = local.rotation;
                const f32 time = static_cast<f32>(t);
                track.translations.push_back({time, local.translation});
                track.rotations.push_back({time, local.rotation});
                track.scales.push_back({time, local.scale});
            }
        }
        Clip& clip = out[written];
        delete clip.impl;
        clip.impl = new Clip::Impl();
        clip.impl->animation = ozz::animation::offline::AnimationBuilder()(raw);
        if (!clip.impl->animation) {
            LSD_LOG_WARN("anim", "%s: animation '%s' could not be built", path, stack->name.data);
            continue;
        }
        std::snprintf(clip.name, sizeof clip.name, "%s", stack->name.length ? stack->name.data : "Take");
        clip.duration = raw.duration;
        // Morph target weights the take animates (DeformPercent 0..100 in the FBX), matched by name later.
        for (const ufbx_blend_channel* channel : scene->blend_channels) {
            Clip::Impl::MorphCurve curve{channel->name.data, std::vector<f32>(frames)};
            bool moves = false;
            for (u32 f = 0; f < frames; ++f) {
                const f64 t = f + 1 == frames ? length : static_cast<f64>(f) / 30.0;
                curve.weights[f] = static_cast<f32>(ufbx_evaluate_blend_weight(stack->anim, channel, begin + t));
                moves = moves || curve.weights[f] != curve.weights[0];
            }
            if (moves || curve.weights[0] != 0.0f) clip.impl->morphs.push_back(std::move(curve));
        }
        ++written;
    }
    ufbx_free_scene(scene);
    return written;
}

struct Pose::Impl {
    ozz::animation::SamplingJob::Context context;
    std::vector<ozz::math::SoaTransform> locals;
    // inertialize(): last frame's output and the offset easing out since the last jump.
    std::vector<ozz::math::SoaTransform> last, offset;
    f32 offset_age = 0.0f;
    // mirror(): the joint pairs and rest corrections, made once per model and facing.
    const SkinnedModel* mirror_model = nullptr;
    f32 mirror_yaw = 0.0f;
    std::vector<i32> mirror_of;
    std::vector<ozz::math::Float4x4> mirror_fix, mirrored;
    std::vector<ozz::math::Float4x4> models;
    // Blending: one sampled pose per input.
    ozz::animation::SamplingJob::Context contexts[kMaxBlendInputs];
    std::vector<ozz::math::SoaTransform> layers[kMaxBlendInputs];
    // layer(): the clip laid over, the pose under it and each one's per-joint weights.
    ozz::animation::SamplingJob::Context over_context;
    std::vector<ozz::math::SoaTransform> over, base;
    std::vector<ozz::math::SimdFloat4> over_weights, base_weights;
};

Pose::~Pose() {
    delete impl_;
    delete[] rows_;
}

void Pose::sample(const SkinnedModel& model, const Clip* clip, f32 time, bool looping) {
    f32 ratio = clip && clip->duration > 0.0f ? time / clip->duration : 0.0f;
    ratio = looping ? ratio - std::floor(ratio) : (ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio);
    const BlendInput input{clip, ratio, 1.0f};
    sample_blend(model, &input, 1);
}

bool Pose::inertialize(const SkinnedModel& model, bool switched, f32 dt, f32 half_life) {
    if (!impl_ || impl_->locals.empty()) return false;
    namespace m = ozz::math;
    using ::operator+;  // ozz's SoA operators are global; lsd's own (Vec3...) hide them in here
    using ::operator-;
    using ::operator*;
    std::vector<m::SoaTransform>& now = impl_->locals;
    std::vector<m::SoaTransform>& off = impl_->offset;
    if (switched && impl_->last.size() == now.size()) {
        off.resize(now.size());
        for (usize i = 0; i < now.size(); ++i) {
            off[i].translation = impl_->last[i].translation - now[i].translation;
            off[i].scale = impl_->last[i].scale - now[i].scale;
            m::SoaQuaternion q = impl_->last[i].rotation * m::Conjugate(now[i].rotation);
            const m::SimdFloat4 sign = m::Select(m::CmpLt(q.w, m::simd_float4::zero()), m::simd_float4::Load1(-1.0f), m::simd_float4::one());
            q = {q.x * sign, q.y * sign, q.z * sign, q.w * sign};  // the short way round
            off[i].rotation = q;
        }
        impl_->offset_age = 0.0f;
    }
    bool changed = false;
    if (!off.empty()) {
        impl_->offset_age += dt;
        const f32 left = half_life > 0.0f ? std::exp2(-impl_->offset_age / half_life) : 0.0f;
        if (left < 1e-3f) {
            off.clear();
        } else {
            const m::SimdFloat4 k = m::simd_float4::Load1(left);
            const m::SoaQuaternion identity = m::SoaQuaternion::identity();
            for (usize i = 0; i < now.size(); ++i) {
                now[i].translation = now[i].translation + off[i].translation * k;
                now[i].scale = now[i].scale + off[i].scale * k;
                now[i].rotation = m::NLerp(identity, off[i].rotation, k) * now[i].rotation;
            }
            changed = true;
        }
    }
    impl_->last = now;
    return changed ? rebuild(model, -1) : true;
}

namespace {

// The joint's name with left and right swapped ("" when it has no side).
std::string other_side(const char* name) {
    std::string s = name;
    const struct { const char* a; const char* b; } pairs[] = {{"Left", "Right"}, {"left", "right"}, {"LEFT", "RIGHT"}, {"_l_", "_r_"}, {"_L_", "_R_"}, {".l", ".r"}, {".L", ".R"}};
    for (const auto& p : pairs) {
        for (int way = 0; way < 2; ++way) {
            const char* from = way ? p.b : p.a;
            const char* to = way ? p.a : p.b;
            const usize at = s.find(from);
            if (at != std::string::npos) return s.replace(at, std::strlen(from), to);
        }
    }
    auto ends = [&](const char* suf) { return s.size() > 2 && s.compare(s.size() - 2, 2, suf) == 0; };
    if (ends("_l") || ends("_L")) return s.substr(0, s.size() - 1) + (s.back() == 'l' ? "r" : "R");
    if (ends("_r") || ends("_R")) return s.substr(0, s.size() - 1) + (s.back() == 'r' ? "l" : "L");
    auto starts = [&](const char* pre) { return s.compare(0, 2, pre) == 0; };
    if (starts("l_") || starts("L_")) return (s[0] == 'l' ? "r" : "R") + s.substr(1);
    if (starts("r_") || starts("R_")) return (s[0] == 'r' ? "l" : "L") + s.substr(1);
    return "";
}

} // namespace

bool Pose::mirror(const SkinnedModel& model, f32 forward_yaw) {
    if (!impl_ || impl_->models.empty() || !model.impl || !model.impl->skeleton) return false;
    namespace m = ozz::math;
    using ::operator*;
    const auto& skeleton = *model.impl->skeleton;
    const u32 joints = joints_;
    const m::SimdFloat4 l = m::simd_float4::Load(-std::sin(forward_yaw), std::cos(forward_yaw), 0.0f, 0.0f);  // sideways
    // R = I - 2 l l^T: the reflection across the forward / up plane (its own inverse).
    m::Float4x4 reflect;
    for (int c = 0; c < 3; ++c) {
        f32 col[4];
        const f32 lc = c == 0 ? -std::sin(forward_yaw) : c == 1 ? std::cos(forward_yaw) : 0.0f;
        m::StorePtrU(l * m::simd_float4::Load1(-2.0f * lc), col);
        col[c] += 1.0f;
        col[3] = 0.0f;
        reflect.cols[c] = m::simd_float4::LoadPtrU(col);
    }
    reflect.cols[3] = m::simd_float4::w_axis();
    if (impl_->mirror_model != &model || impl_->mirror_yaw != forward_yaw || impl_->mirror_of.size() != joints) {
        impl_->mirror_model = &model;
        impl_->mirror_yaw = forward_yaw;
        impl_->mirror_of.assign(joints, -1);
        for (u32 j = 0; j < joints; ++j) {
            const std::string partner = other_side(model.joint_name(j));
            const i32 k = partner.empty() ? -1 : model.find_joint(partner.c_str());
            impl_->mirror_of[j] = k >= 0 ? k : static_cast<i32>(j);
        }
        // The bind pose in model space (the T / A pose it was skinned in: symmetric, unlike many
        // animation rest poses), and per joint the correction that maps it onto itself:
        // fix_j = (R * bind_partner * R)^-1 * bind_j.
        std::vector<m::Float4x4> rest_models(joints);
        for (u32 j = 0; j < joints; ++j) {
            const Mat4 bind = j < model.inverse_bind.size() ? affine_inverse(model.inverse_bind[j]) : Mat4{};
            for (int c = 0; c < 4; ++c) rest_models[j].cols[c] = m::simd_float4::LoadPtrU(&bind.m[c * 4]);
        }
        impl_->mirror_fix.resize(joints);
        for (u32 j = 0; j < joints; ++j) {
            impl_->mirror_fix[j] = m::Invert(reflect * rest_models[static_cast<usize>(impl_->mirror_of[j])] * reflect) * rest_models[j];
        }
    }
    auto& out = impl_->mirrored;
    out.resize(joints);
    for (u32 j = 0; j < joints; ++j) out[j] = reflect * impl_->models[static_cast<usize>(impl_->mirror_of[j])] * reflect * impl_->mirror_fix[j];
    // Back to local transforms (parent^-1 * model), packed four joints at a time.
    const auto parents = skeleton.joint_parents();
    for (u32 soa = 0; soa < impl_->locals.size(); ++soa) {
        f32 t[3][4] = {}, q[4][4] = {}, s[3][4] = {};
        for (u32 lane = 0; lane < 4; ++lane) {
            const u32 j = soa * 4 + lane;
            q[3][lane] = 1.0f;
            s[0][lane] = s[1][lane] = s[2][lane] = 1.0f;
            if (j >= joints) continue;
            const i16 parent = parents[j];
            const m::Float4x4 local = parent >= 0 ? m::Invert(out[static_cast<usize>(parent)]) * out[j] : out[j];
            m::SimdFloat4 tt, qq, ss;
            if (!m::ToAffine(local, &tt, &qq, &ss)) continue;
            f32 a[4], b[4], c[4];
            m::StorePtrU(tt, a);
            m::StorePtrU(qq, b);
            m::StorePtrU(ss, c);
            for (int k = 0; k < 3; ++k) {
                t[k][lane] = a[k];
                s[k][lane] = c[k];
            }
            for (int k = 0; k < 4; ++k) q[k][lane] = b[k];
        }
        m::SoaTransform& x = impl_->locals[soa];
        x.translation = {m::simd_float4::LoadPtrU(t[0]), m::simd_float4::LoadPtrU(t[1]), m::simd_float4::LoadPtrU(t[2])};
        x.rotation = {m::simd_float4::LoadPtrU(q[0]), m::simd_float4::LoadPtrU(q[1]), m::simd_float4::LoadPtrU(q[2]), m::simd_float4::LoadPtrU(q[3])};
        x.scale = {m::simd_float4::LoadPtrU(s[0]), m::simd_float4::LoadPtrU(s[1]), m::simd_float4::LoadPtrU(s[2])};
    }
    return rebuild(model, -1);
}

void Pose::sample_blend(const SkinnedModel& model, const BlendInput* inputs, u32 count) {
    if (!model.impl || !model.impl->skeleton) return;
    const auto& skeleton = *model.impl->skeleton;
    const u32 joints = static_cast<u32>(skeleton.num_joints());
    if (!impl_) impl_ = new Impl();
    if (joints != joints_) {
        delete[] rows_;
        rows_ = new f32[joints * 12];
        joints_ = joints;
        impl_->locals.resize(static_cast<usize>(skeleton.num_soa_joints()));
        impl_->models.resize(joints);
    }
    const auto rest = skeleton.joint_rest_poses();
    // Each input sampled into its own layer (or the rest pose), then blended by weight.
    auto sample_into = [&](const BlendInput& in, ozz::animation::SamplingJob::Context& context, std::vector<ozz::math::SoaTransform>& out) {
        out.resize(impl_->locals.size());
        const ozz::animation::Animation* animation = in.clip && in.clip->impl ? in.clip->impl->animation.get() : nullptr;
        if (animation && animation->num_tracks() == skeleton.num_joints()) {
            context.Resize(animation->num_tracks());
            ozz::animation::SamplingJob sampling;
            sampling.animation = animation;
            sampling.context = &context;
            sampling.ratio = in.ratio < 0.0f ? 0.0f : in.ratio > 1.0f ? 1.0f : in.ratio;
            sampling.output = ozz::make_span(out);
            if (sampling.Run()) return;
        }
        for (usize i = 0; i < out.size(); ++i) out[i] = rest[i];
    };
    count = count < kMaxBlendInputs ? count : kMaxBlendInputs;
    if (count <= 1) {
        const BlendInput none{};
        sample_into(count ? inputs[0] : none, impl_->context, impl_->locals);
    } else {
        ozz::animation::BlendingJob::Layer layers[kMaxBlendInputs];
        f32 total = 0.0f;
        for (u32 i = 0; i < count; ++i) total += inputs[i].weight > 0.0f ? inputs[i].weight : 0.0f;
        for (u32 i = 0; i < count; ++i) {
            sample_into(inputs[i], impl_->contexts[i], impl_->layers[i]);
            layers[i].weight = total > 0.0f ? (inputs[i].weight > 0.0f ? inputs[i].weight : 0.0f) / total : (i == 0 ? 1.0f : 0.0f);
            layers[i].transform = ozz::make_span(impl_->layers[i]);
        }
        ozz::animation::BlendingJob blend;
        blend.threshold = 0.01f;
        blend.layers = ozz::span<const ozz::animation::BlendingJob::Layer>(layers, count);
        blend.rest_pose = rest;
        blend.output = ozz::make_span(impl_->locals);
        if (!blend.Run()) return;
    }
    rebuild(model, -1);
}

bool Pose::layer(const SkinnedModel& model, const Clip* clip, f32 ratio, f32 weight, i32 from_joint) {
    if (!impl_ || !model.impl || !model.impl->skeleton || !clip || !clip->impl || weight <= 0.0f) return false;
    const auto& skeleton = *model.impl->skeleton;
    const ozz::animation::Animation* animation = clip->impl->animation.get();
    const usize soa = impl_->locals.size();
    if (!animation || animation->num_tracks() != skeleton.num_joints() || soa != static_cast<usize>(skeleton.num_soa_joints())) return false;
    impl_->over.resize(soa);
    impl_->over_context.Resize(animation->num_tracks());
    ozz::animation::SamplingJob sampling;
    sampling.animation = animation;
    sampling.context = &impl_->over_context;
    sampling.ratio = ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
    sampling.output = ozz::make_span(impl_->over);
    if (!sampling.Run()) return false;
    // The joints it covers: from_joint and everything under it (parents come before their children).
    const auto parents = skeleton.joint_parents();
    std::vector<f32> mask(soa * 4, 0.0f);
    for (usize j = 0; j < static_cast<usize>(skeleton.num_joints()); ++j) {
        const i32 parent = parents[j];
        mask[j] = from_joint < 0 || static_cast<i32>(j) == from_joint || (parent >= 0 && mask[static_cast<usize>(parent)] > 0.0f) ? 1.0f : 0.0f;
    }
    const f32 w = weight > 1.0f ? 1.0f : weight;
    impl_->base = impl_->locals;
    impl_->over_weights.resize(soa);
    impl_->base_weights.resize(soa);
    for (usize s = 0; s < soa; ++s) {
        const f32* m = &mask[s * 4];
        impl_->over_weights[s] = ozz::math::simd_float4::Load(w * m[0], w * m[1], w * m[2], w * m[3]);
        impl_->base_weights[s] = ozz::math::simd_float4::Load(1.0f - w * m[0], 1.0f - w * m[1], 1.0f - w * m[2], 1.0f - w * m[3]);
    }
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].weight = layers[1].weight = 1.0f;
    layers[0].transform = ozz::make_span(impl_->base);
    layers[0].joint_weights = ozz::make_span(impl_->base_weights);
    layers[1].transform = ozz::make_span(impl_->over);
    layers[1].joint_weights = ozz::make_span(impl_->over_weights);
    ozz::animation::BlendingJob blend;
    blend.threshold = 0.01f;
    blend.layers = ozz::span<const ozz::animation::BlendingJob::Layer>(layers, 2);
    blend.rest_pose = skeleton.joint_rest_poses();
    blend.output = ozz::make_span(impl_->locals);
    return blend.Run() && rebuild(model, -1);
}

i32 SkinnedModel::motion_root() const {
    const i32 named = find_joint("root");  // Unreal rigs: the bone made for it, under the import's Root
    if (named >= 0) return named;
    for (u32 j = 1; j < joint_count(); ++j) {
        if (joint_parent(j) == 0) return static_cast<i32>(j);  // joint 0 is the import's Root (axes, scale), which never moves
    }
    return joint_count() ? 0 : -1;
}

Vec3 Pose::strip_root(const SkinnedModel& model) {
    if (!impl_ || impl_->locals.empty() || !model.impl || !model.impl->skeleton) return {};
    const i32 r = model.motion_root();
    if (r < 0 || static_cast<u32>(r) >= joints_) return {};
    const u32 j = static_cast<u32>(r), lane = j % 4;
    const i32 parent = model.joint_parent(j);
    const Mat4 to_model = parent >= 0 ? joint_model(static_cast<u32>(parent)) : Mat4{};  // the bone's local space -> model space
    ozz::math::SoaFloat3& t = impl_->locals[j / 4].translation;
    const ozz::math::SoaFloat3& rest = model.impl->skeleton->joint_rest_poses()[j / 4].translation;
    float x[4], y[4], z[4], rx[4], ry[4], rz[4];
    ozz::math::StorePtrU(t.x, x);
    ozz::math::StorePtrU(t.y, y);
    ozz::math::StorePtrU(t.z, z);
    ozz::math::StorePtrU(rest.x, rx);
    ozz::math::StorePtrU(rest.y, ry);
    ozz::math::StorePtrU(rest.z, rz);
    const Vec3 local_move{x[lane] - rx[lane], y[lane] - ry[lane], z[lane] - rz[lane]};
    const Vec3 in_model = transform_vector(to_model, local_move);
    const Vec3 moved{in_model.x, in_model.y, 0.0f};  // sideways only: height stays the animation's
    const Vec3 back = transform_vector(affine_inverse(to_model), moved);
    x[lane] -= back.x;
    y[lane] -= back.y;
    z[lane] -= back.z;
    t.x = ozz::math::simd_float4::LoadPtrU(x);
    t.y = ozz::math::simd_float4::LoadPtrU(y);
    t.z = ozz::math::simd_float4::LoadPtrU(z);
    rebuild(model, -1);
    return moved;
}

bool Pose::rebuild(const SkinnedModel& model, i32 from) {
    ozz::animation::LocalToModelJob to_model;
    to_model.skeleton = model.impl->skeleton.get();
    to_model.input = ozz::make_span(impl_->locals);
    to_model.output = ozz::make_span(impl_->models);
    to_model.from = from < 0 ? ozz::animation::Skeleton::kNoParent : from;
    if (!to_model.Run()) return false;
    for (u32 j = 0; j < joints_; ++j) {
        const Mat4 skin = joint_model(j) * model.inverse_bind[j];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) rows_[j * 12 + r * 4 + c] = skin.at(r, c);
        }
    }
    return true;
}

namespace {

// Multiplies one joint's local rotation (inside its SoA group of four) by `q`.
void rotate_local(std::vector<ozz::math::SoaTransform>& locals, u32 joint, const ozz::math::SimdQuaternion& q) {
    ozz::math::SoaTransform& soa = locals[joint / 4];
    ozz::math::SimdQuaternion aos[4];
    ozz::math::Transpose4x4(&soa.rotation.x, &aos->xyzw);
    aos[joint & 3] = aos[joint & 3] * q;
    ozz::math::Transpose4x4(&aos->xyzw, &soa.rotation.x);
}

ozz::math::SimdFloat4 point(Vec3 v) { return ozz::math::simd_float4::Load(v.x, v.y, v.z, 1.0f); }
ozz::math::SimdFloat4 direction(Vec3 v) { return ozz::math::simd_float4::Load(v.x, v.y, v.z, 0.0f); }
Vec3 translation_of(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

} // namespace

bool Pose::two_bone_ik(const SkinnedModel& model, u32 end, Vec3 target, f32 weight) {
    if (!impl_ || end >= joints_) return false;
    const i32 mid = model.joint_parent(end);
    const i32 start = mid >= 0 ? model.joint_parent(static_cast<u32>(mid)) : -1;
    if (start < 0) return false;
    const Vec3 a = translation_of(joint_model(static_cast<u32>(start))), b = translation_of(joint_model(static_cast<u32>(mid))), c = translation_of(joint_model(end));
    if (length(a - b) < 1e-5f || length(c - b) < 1e-5f) return false;  // a zero-length bone has no bend
    // The bend axis as the pose already bends (works for any rig), in the mid joint's own space.
    Vec3 axis = cross(a - b, c - b);
    if (length(axis) < 1e-6f) axis = cross(a - b, Vec3{0, 0, 1});  // straight: any axis across the chain
    if (length(axis) < 1e-6f) axis = cross(a - b, Vec3{1, 0, 0});
    const Mat4 to_mid = inverse(joint_model(static_cast<u32>(mid)));
    Vec3 local_axis = normalize(Vec3{to_mid.at(0, 0) * axis.x + to_mid.at(0, 1) * axis.y + to_mid.at(0, 2) * axis.z,
                                     to_mid.at(1, 0) * axis.x + to_mid.at(1, 1) * axis.y + to_mid.at(1, 2) * axis.z,
                                     to_mid.at(2, 0) * axis.x + to_mid.at(2, 1) * axis.y + to_mid.at(2, 2) * axis.z});
    Vec3 pole = b - (a + c) * 0.5f;  // keep bending the same way (knees forward)
    if (length(pole) < 1e-5f) pole = cross(normalize(axis), c - a);  // straight: bend across the axis
    ozz::math::SimdQuaternion start_fix, mid_fix;
    bool reached = false;
    ozz::animation::IKTwoBoneJob job;
    job.target = point(target);
    job.pole_vector = direction(normalize(pole));
    job.mid_axis = direction(local_axis);
    job.weight = weight < 0.0f ? 0.0f : weight > 1.0f ? 1.0f : weight;
    job.soften = 0.97f;  // no snapping straight at full reach
    job.start_joint = &impl_->models[static_cast<usize>(start)];
    job.mid_joint = &impl_->models[static_cast<usize>(mid)];
    job.end_joint = &impl_->models[end];
    job.start_joint_correction = &start_fix;
    job.mid_joint_correction = &mid_fix;
    job.reached = &reached;
    if (!job.Run()) return false;
    rotate_local(impl_->locals, static_cast<u32>(start), start_fix);
    rotate_local(impl_->locals, static_cast<u32>(mid), mid_fix);
    return rebuild(model, start);
}

bool Pose::aim_ik(const SkinnedModel& model, u32 joint, Vec3 target, Vec3 forward, f32 weight) {
    if (!impl_ || joint >= joints_) return false;
    const Vec3 f = normalize(forward);
    const Vec3 up = std::fabs(f.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    ozz::math::SimdQuaternion fix;
    ozz::animation::IKAimJob job;
    job.target = point(target);
    job.forward = direction(f);
    job.up = direction(up);
    job.pole_vector = direction(Vec3{0, 0, 1});
    job.weight = weight < 0.0f ? 0.0f : weight > 1.0f ? 1.0f : weight;
    job.joint = &impl_->models[joint];
    job.joint_correction = &fix;
    if (!job.Run()) return false;
    rotate_local(impl_->locals, joint, fix);
    return rebuild(model, static_cast<i32>(joint));
}

bool Pose::follow(const SkinnedModel& model, const SkinnedModel& leader_model, const Pose& leader) {
    const u32 joints = model.joint_count();
    if (!joints || !leader.impl_ || model.inverse_bind.size() < joints) return false;
    if (!impl_) impl_ = new Impl();
    if (joints != joints_) {
        delete[] rows_;
        rows_ = new f32[joints * 12];
        joints_ = joints;
    }
    impl_->models.resize(joints);
    for (u32 j = 0; j < joints; ++j) {
        const char* name = model.joint_name(j);
        // The same index first: skeletons exported alike line up (and repeated names stay apart).
        const i32 l = j < leader_model.joint_count() && std::strcmp(leader_model.joint_name(j), name) == 0 ? static_cast<i32>(j) : leader_model.find_joint(name);
        const Mat4 joint = l >= 0 ? leader.joint_model(static_cast<u32>(l)) : affine_inverse(model.inverse_bind[j]);  // missing: its bind pose
        for (int c = 0; c < 4; ++c) impl_->models[j].cols[c] = ozz::math::simd_float4::LoadPtrU(&joint.m[c * 4]);
        const Mat4 skin = joint * model.inverse_bind[j];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) rows_[j * 12 + r * 4 + c] = skin.at(r, c);
        }
    }
    return true;
}

Mat4 Pose::joint_model(u32 joint) const {
    Mat4 m;
    if (!impl_ || joint >= impl_->models.size()) return m;
    for (int c = 0; c < 4; ++c) ozz::math::StorePtrU(impl_->models[joint].cols[c], &m.m[c * 4]);
    return m;
}

} // namespace lsd::anim
