// Animation tests: skinned FBX files from ufbx's test data. Our import + ozz sampling + skinning
// must put vertices where ufbx's own skinning does, at rest and part-way through an animation.
#include "lsd/animation/animation.h"

#include <ufbx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

// Skins a vertex the way the shader does: weighted sum of the joints' rows.
static Vec3 skin(const anim::Pose& pose, const rhi::SkinnedVertex& v) {
    f32 r[12] = {};
    for (int k = 0; k < 4; ++k) {
        const f32 w = static_cast<f32>(v.weights[k]) / 255.0f;
        for (int i = 0; i < 12; ++i) r[i] += pose.joint_rows()[v.joints[k] * 12 + i] * w;
    }
    const f32* p = v.position;
    return {r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3], r[4] * p[0] + r[5] * p[1] + r[6] * p[2] + r[7], r[8] * p[0] + r[9] * p[1] + r[10] * p[2] + r[11]};
}

// Largest distance between our skinned triangle corners and ufbx's, at `time` (negative: rest).
static f32 worst_error(const char* path, const anim::SkinnedModel& model, const anim::Clip* clip, f64 time) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;
    ufbx_scene* scene = ufbx_load_file(path, &opts, nullptr);
    if (!scene) return 1e9f;
    ufbx_scene* posed = scene;
    if (time >= 0.0 && scene->anim_stacks.count) posed = ufbx_evaluate_scene(scene, scene->anim_stacks.data[0]->anim, scene->anim_stacks.data[0]->time_begin + time, nullptr, nullptr);
    anim::Pose pose;
    pose.sample(model, time >= 0.0 ? clip : nullptr, time >= 0.0 ? static_cast<f32>(time) : 0.0f, false);
    // Our vertices are the skinned meshes' triangle corners in order, the first mesh first.
    f32 worst = 0.0f;
    usize corner = 0;
    std::vector<u32> tri;
    for (usize m = 0; m < posed->meshes.count && corner < model.vertices.size(); ++m) {
        const ufbx_mesh* mesh = posed->meshes.data[m];
        if (!mesh->skin_deformers.count) continue;
        const ufbx_skin_deformer* skin_deformer = mesh->skin_deformers.data[0];
        tri.resize(mesh->max_face_triangles * 3);
        for (usize f = 0; f < mesh->faces.count; ++f) {
            const u32 count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[f]);
            for (u32 i = 0; i < count * 3 && corner < model.vertices.size(); ++i, ++corner) {
                const u32 vertex = mesh->vertex_indices.data[tri[i]];
                const ufbx_matrix m4 = ufbx_get_skin_vertex_matrix(skin_deformer, vertex, &mesh->instances.data[0]->geometry_to_world);
                const ufbx_vec3 expected = ufbx_transform_position(&m4, mesh->vertices.data[vertex]);
                const Vec3 ours = skin(pose, model.vertices[corner]);
                const Vec3 d = ours - Vec3{static_cast<f32>(expected.x), static_cast<f32>(expected.y), static_cast<f32>(expected.z)};
                worst = std::fmax(worst, length(d));
                if (std::getenv("LSD_ANIM_DEBUG") && corner < 6) std::printf("      corner %zu ours %.3f %.3f %.3f expected %.3f %.3f %.3f weights %u %u joints %u %u\n", corner, ours.x, ours.y, ours.z, expected.x, expected.y, expected.z, model.vertices[corner].weights[0], model.vertices[corner].weights[1], model.vertices[corner].joints[0], model.vertices[corner].joints[1]);
            }
        }
    }
    if (posed != scene) ufbx_free_scene(posed);
    ufbx_free_scene(scene);
    return worst;
}

int main() {
    const std::string data = LSD_UFBX_DATA_DIR;
    const char* files[] = {"max_transformed_skin_6100_binary.fbx", "maya_advanced_skinned_pivot_7700_binary.fbx", "blender_293_half_skinned_7400_binary.fbx"};
    u32 tested_animation = 0;
    for (const char* name : files) {
        const std::string path = data + "/" + name;
        anim::SkinnedModel model(default_allocator());
        char error[256] = "";
        const bool ok = anim::import_skinned(path.c_str(), 1.0f, model, error, sizeof error);
        CHECK(ok);
        if (!ok) {
            std::printf("  %s: %s\n", name, error);
            continue;
        }
        anim::Clip clips[4];
        const u32 clip_count = anim::import_clips(path.c_str(), model, clips, 4);
        const f32 size = length(model.bounds_max - model.bounds_min);
        const f32 rest = worst_error(path.c_str(), model, nullptr, -1.0);
        std::printf("  %s: %u joints, %zu corners, %u clips, size %.3f, rest error %.5f\n", name, model.joint_count(), model.vertices.size(),
                    clip_count, static_cast<double>(size), static_cast<double>(rest));
        CHECK(model.joint_count() > 1 && model.vertices.size() >= 3 && size > 0.0f);
        CHECK(rest < size * 0.01f);  // weights are bytes: a little rounding is allowed
        if (clip_count) {
            const f64 t = std::floor(clips[0].duration * 0.5 * 30.0) / 30.0;  // a sampled frame half-way through
            const f32 moving = worst_error(path.c_str(), model, &clips[0], t);
            std::printf("    clip '%s' %.2f s, error at %.2f s: %.5f\n", clips[0].name, static_cast<double>(clips[0].duration), t, static_cast<double>(moving));
            CHECK(moving < size * 0.01f);
            ++tested_animation;
        }
    }
    CHECK(tested_animation > 0);  // at least one file must exercise animation
    if (g_failures == 0) std::printf("All animation tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
