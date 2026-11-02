// Animation tests: skinned FBX files from ufbx's test data. Our import + ozz sampling + skinning
// must put vertices where ufbx's own skinning does, at rest and part-way through an animation.
#include "lsd/animation/anim_graph.h"

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

// The graph's state machine and blend space, without any real animation data.
static void graph_tests() {
    anim::Clip clips[3];
    clips[0].duration = 2.0f;  // idle
    clips[1].duration = 1.0f;  // walk
    clips[2].duration = 0.5f;  // run
    anim::AnimGraph g;
    g.states.count = 2;
    std::snprintf(g.states[0].name, 32, "Idle");
    g.states[0].clip = 0;
    std::snprintf(g.states[1].name, 32, "Move");
    std::snprintf(g.states[1].blend_parameter, 32, "Speed");
    g.states[1].samples.count = 2;
    g.states[1].samples[0] = {2, 5.0f};  // out of order on purpose
    g.states[1].samples[1] = {1, 1.0f};
    g.transitions.count = 2;
    g.transitions[0] = {};
    std::snprintf(g.transitions[0].from, 32, "Idle");
    std::snprintf(g.transitions[0].to, 32, "Move");
    g.transitions[0].condition = 0;  // Speed > 0.1
    g.transitions[0].blend_time = 0.2f;
    g.transitions[1] = {};
    std::snprintf(g.transitions[1].from, 32, "Move");
    std::snprintf(g.transitions[1].to, 32, "Idle");
    g.transitions[1].condition = 1;  // Speed < 0.1

    anim::GraphState gs;
    anim::BlendInput in[anim::kMaxBlendInputs];
    u32 n = anim::step_graph(g, clips, 3, gs, 0.5f, in);
    CHECK(n == 1 && in[0].clip == &clips[0] && std::strcmp(anim::state_name(g, gs), "Idle") == 0);
    CHECK(std::fabs(gs.phase - 0.25f) < 1e-4f);  // 0.5 s of a 2 s clip

    gs.set("Speed", 3.0f);  // halfway between walk (1) and run (5)
    n = anim::step_graph(g, clips, 3, gs, 0.1f, in);
    CHECK(std::strcmp(anim::state_name(g, gs), "Move") == 0);
    CHECK(n == 3);  // walk + run fading in, idle fading out
    f32 total = 0.0f, walk = 0.0f, run = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        total += in[i].weight;
        if (in[i].clip == &clips[1]) walk += in[i].weight;
        if (in[i].clip == &clips[2]) run += in[i].weight;
    }
    CHECK(std::fabs(total - 1.0f) < 1e-4f && std::fabs(walk - run) < 1e-4f && walk > 0.0f);

    n = anim::step_graph(g, clips, 3, gs, 0.2f, in);  // the crossfade is over
    CHECK(n == 2 && gs.previous == -1);
    CHECK(std::fabs(in[0].weight + in[1].weight - 1.0f) < 1e-4f);

    gs.set("Speed", 9.0f);  // past the last sample: run alone
    n = anim::step_graph(g, clips, 3, gs, 0.0f, in);
    CHECK(n == 1 && in[0].clip == &clips[2]);

    gs.set("Speed", 0.0f);  // back to idle, instantly (blend_time 0.2 default -> crossfade)
    anim::step_graph(g, clips, 3, gs, 0.0f, in);
    CHECK(std::strcmp(anim::state_name(g, gs), "Idle") == 0 && gs.previous == 1);

    // Finished: a one-shot state hands back when its clip ends.
    anim::AnimGraph once;
    once.states.count = 2;
    std::snprintf(once.states[0].name, 32, "Jump");
    once.states[0].looping = false;
    once.states[0].clip = 1;
    std::snprintf(once.states[1].name, 32, "Land");
    once.transitions.count = 1;
    once.transitions[0] = {};
    std::snprintf(once.transitions[0].to, 32, "Land");
    once.transitions[0].condition = 4;
    anim::GraphState os;
    anim::step_graph(once, clips, 3, os, 0.6f, in);
    CHECK(std::strcmp(anim::state_name(once, os), "Jump") == 0 && !os.finished);
    anim::step_graph(once, clips, 3, os, 0.6f, in);
    CHECK(os.finished);
    anim::step_graph(once, clips, 3, os, 0.0f, in);
    CHECK(std::strcmp(anim::state_name(once, os), "Land") == 0);

    // 2D blend space (Speed x Direction): four clips on a square. Inside, the weights rebuild the
    // point (barycentric); outside, the nearest edge; on a corner, that clip alone.
    anim::Clip four[4];
    for (anim::Clip& c : four) c.duration = 1.0f;
    anim::AnimGraph strafe;
    strafe.states.count = 1;
    anim::AnimState& st = strafe.states[0];
    std::snprintf(st.blend_parameter, 32, "Speed");
    std::snprintf(st.blend_parameter_y, 32, "Direction");
    st.samples.count = 4;
    st.samples[0] = {0, 0.0f, 0.0f};
    st.samples[1] = {1, 4.0f, 0.0f};
    st.samples[2] = {2, 0.0f, 90.0f};
    st.samples[3] = {3, 4.0f, 90.0f};
    anim::GraphState ss;
    auto blended = [&](f32 speed, f32 direction, f32& x, f32& y) {
        ss.set("Speed", speed);
        ss.set("Direction", direction);
        const u32 count = anim::step_graph(strafe, four, 4, ss, 0.0f, in);
        f32 total = 0.0f;
        x = y = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            const auto& sample = st.samples[static_cast<u32>(in[i].clip - four)];
            x += sample.position * in[i].weight;
            y += sample.position_y * in[i].weight;
            total += in[i].weight;
        }
        CHECK(std::fabs(total - 1.0f) < 1e-4f && count >= 1 && count <= 3);
        return count;
    };
    f32 x, y;
    blended(1.0f, 30.0f, x, y);
    CHECK(std::fabs(x - 1.0f) < 1e-3f && std::fabs(y - 30.0f) < 1e-3f);
    CHECK(blended(8.0f, 45.0f, x, y) == 2 && std::fabs(x - 4.0f) < 1e-3f && std::fabs(y - 45.0f) < 1e-3f);  // past the right edge
    CHECK(blended(4.0f, 90.0f, x, y) == 1 && in[0].clip == &four[3]);
}

// Morph targets: offsets mixed in by weight, by name, several targets sharing a name together.
static void morph_tests(const std::string& data) {
    anim::SkinnedModel model(default_allocator());
    model.vertices.resize(2);
    for (rhi::SkinnedVertex& v : model.vertices) v = rhi::SkinnedVertex{};
    model.vertices[1].position[0] = 1.0f;
    model.morph_deltas.push_back({1, {0, 0, 2}});
    model.morph_deltas.push_back({0, {0, 1, 0}});
    model.morphs.push_back({"Up", 0, 1});
    model.morphs.push_back({"Up", 1, 1});  // the same name on another mesh
    rhi::SkinnedVertex out[2];
    anim::apply_morphs(model, [](const char* name) { return std::strcmp(name, "Up") == 0 ? 0.5f : 0.0f; }, out);
    CHECK(out[1].position[0] == 1.0f && out[1].position[2] == 1.0f && out[0].position[1] == 0.5f);
    anim::apply_morphs(model, [](const char*) { return 0.0f; }, out);
    CHECK(out[1].position[2] == 0.0f && out[0].position[1] == 0.0f);

    // A real rig: Blender shape keys arrive with their names and offsets on our vertices.
    u32 found = 0;
    for (const char* name : {"blender_293_barbarian_7400_binary.fbx"}) {
        anim::SkinnedModel m(default_allocator());
        char error[256] = "";
        CHECK(anim::import_skinned((data + "/" + name).c_str(), 1.0f, m, error, sizeof error));
        std::printf("  %s: %zu morph targets, %zu offsets\n", name, m.morphs.size(), m.morph_deltas.size());
        for (const anim::MorphTarget& t : m.morphs) {
            CHECK(t.name[0] && t.count > 0 && t.first + t.count <= m.morph_deltas.size());
            for (u32 i = t.first; i < t.first + t.count; ++i) CHECK(m.morph_deltas[i].vertex < m.vertices.size());
        }
        found += static_cast<u32>(m.morphs.size());
    }
    CHECK(found > 0);

    // Animated shape-key weights: a take's curve matches ufbx's own evaluation half-way through.
    anim::SkinnedModel rig(default_allocator());
    char error[256] = "";
    const std::string animated = data + "/blender440_shape_weight_anim_7400_binary.fbx";
    CHECK(anim::import_skinned((data + "/blender_293_half_skinned_7400_binary.fbx").c_str(), 1.0f, rig, error, sizeof error));
    anim::Clip clips[2];
    const u32 n = anim::import_clips(animated.c_str(), rig, clips, 2);
    ufbx_load_opts opts{};
    ufbx_scene* scene = ufbx_load_file(animated.c_str(), &opts, nullptr);
    CHECK(n > 0 && scene && scene->blend_channels.count > 0 && scene->anim_stacks.count > 0);
    if (n > 0 && scene && scene->blend_channels.count > 0 && scene->anim_stacks.count > 0) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[0];
        const f64 t = std::floor((stack->time_end - stack->time_begin) * 0.5 * 30.0) / 30.0;  // a sampled frame
        const ufbx_blend_channel* channel = scene->blend_channels.data[0];
        const f32 expected = static_cast<f32>(ufbx_evaluate_blend_weight(stack->anim, channel, stack->time_begin + t));
        f32 ours = -1.0f;
        CHECK(clips[0].morph_weight(channel->name.data, static_cast<f32>(t) / clips[0].duration, ours));
        std::printf("  shape key '%s' at %.2f s: %.3f (ufbx %.3f)\n", channel->name.data, t, static_cast<double>(ours), static_cast<double>(expected));
        CHECK(std::fabs(ours - expected) < 1e-3f);
        const anim::BlendInput half[2] = {{&clips[0], static_cast<f32>(t) / clips[0].duration, 1.0f}, {nullptr, 0.0f, 1.0f}};
        f32 mixed = -1.0f;
        CHECK(anim::blended_morph_weight(half, 2, channel->name.data, mixed) && std::fabs(mixed - expected * 0.5f) < 1e-3f);  // half rest pose
        CHECK(!anim::blended_morph_weight(half, 2, "NoSuchShape", mixed));
    }
    if (scene) ufbx_free_scene(scene);
}

int main() {
    graph_tests();
    const std::string data = LSD_UFBX_DATA_DIR;
    morph_tests(data);
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
        // Two-bone IK: a chain end pulled a little towards its start lands on the target; aim turns a joint.
        for (u32 j = 0; j < model.joint_count(); ++j) {
            const i32 mid = model.joint_parent(j), start = mid >= 0 ? model.joint_parent(static_cast<u32>(mid)) : -1;
            if (start < 0) continue;
            anim::Pose pose;
            pose.sample(model, nullptr, 0.0f, false);
            auto at = [&](u32 k) { const Mat4 m = pose.joint_model(k); return Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)}; };
            const Vec3 a0 = at(static_cast<u32>(start)), c0 = at(j);
            const f32 reach = length(c0 - a0);
            if (reach < size * 0.05f || length(at(static_cast<u32>(mid)) - a0) < size * 0.02f || length(c0 - at(static_cast<u32>(mid))) < size * 0.02f) continue;  // tiny or zero-length bones
            const Vec3 target = a0 + (c0 - a0) * 0.8f + Vec3{0, 0, reach * 0.1f};
            CHECK(pose.two_bone_ik(model, j, target, 1.0f));
            CHECK(length(at(j) - target) < reach * 0.05f);
            CHECK(length(at(static_cast<u32>(start)) - a0) < 1e-4f);  // the chain's root stays put
            const Vec3 look = at(j) + Vec3{0, 0, reach};
            CHECK(pose.aim_ik(model, j, look, Vec3{1, 0, 0}, 1.0f));
            const Mat4 m = pose.joint_model(j);
            const Vec3 forward = normalize(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
            CHECK(dot(forward, Vec3{0, 0, 1}) > 0.99f);  // its +X now points at the target
            break;
        }
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
