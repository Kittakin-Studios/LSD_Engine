// Animation tests: skinned FBX files from ufbx's test data. Our import + ozz sampling + skinning
// must put vertices where ufbx's own skinning does, at rest and part-way through an animation.
#include "lsd/animation/anim_graph.h"
#include "lsd/animation/motion_matching.h"

#include <ufbx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
static void graph_editing_tests() {  // the State Machine window's edits keep transitions in step
    anim::AnimGraph g;
    for (const char* name : {"Idle", "Walk", "Jump"}) {
        anim::AnimState st;
        std::snprintf(st.name, sizeof st.name, "%s", name);
        g.states.push_back(st);
    }
    auto link = [&](const char* from, const char* to) {
        anim::AnimTransition t;
        std::snprintf(t.from, sizeof t.from, "%s", from);
        std::snprintf(t.to, sizeof t.to, "%s", to);
        g.transitions.push_back(t);
    };
    link("Idle", "Walk");
    link("Walk", "Idle");
    link("", "Jump");  // from any state
    std::snprintf(g.entry_state, sizeof g.entry_state, "Idle");
    std::snprintf(g.states[0].name, sizeof g.states[0].name, "Stand");  // renamed in its Details, then:
    anim::rename_state(g, "Idle", g.states[0].name);
    CHECK(std::strcmp(g.transitions[0].from, "Stand") == 0 && std::strcmp(g.transitions[1].to, "Stand") == 0 && std::strcmp(g.entry_state, "Stand") == 0);
    anim::remove_state(g, 1);  // Walk: both transitions with it go, the any-state one stays
    CHECK(g.states.count == 2 && g.transitions.count == 1 && std::strcmp(g.transitions[0].to, "Jump") == 0);
    anim::remove_state(g, 0);  // the entry state
    CHECK(g.entry_state[0] == '\0' && g.states.count == 1);
}

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
    {  // a clip table (clips from several files): entries are pointers, a missing one plays nothing
        const anim::Clip* table[2] = {&clips[2], nullptr};
        anim::AnimGraph one;
        one.states.push_back(anim::AnimState{});
        anim::GraphState os;
        CHECK(anim::step_graph(one, table, 2, os, 0.1f, in) == 1 && in[0].clip == &clips[2]);
        one.states[0].clip = 1;
        anim::GraphState os2;
        CHECK(anim::step_graph(one, table, 2, os2, 0.1f, in) == 1 && in[0].clip == nullptr);
    }
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
    {  // packed for the GPU: cull.hlsl's morph_main run here gives what apply_morphs gives
        Array<u32> packed(default_allocator());
        anim::pack_morphs(model, packed);
        CHECK(packed.size() == 2 * 2 + 2 * 8);
        const f32 weights[2] = {0.5f, 0.25f};
        for (u32 v = 0; v < 2; ++v) {
            f32 p[3];
            std::memcpy(p, model.vertices[v].position, sizeof p);
            for (u32 k = 0; k < packed[v * 2 + 1]; ++k) {
                const u32* e = &packed[2 * 2 + (packed[v * 2] + k) * 8];
                for (u32 c = 0; c < 3; ++c) {
                    f32 o;
                    std::memcpy(&o, &e[c], sizeof o);
                    p[c] += o * weights[e[3]];
                }
            }
            CHECK(v == 1 ? p[2] == 1.0f && p[0] == 1.0f : p[1] == 0.25f);
        }
    }

    // A real rig: Blender shape keys arrive with their names and offsets on our vertices.
    u32 found = 0;
    for (const char* name : {"blender_293_barbarian_7400_binary.fbx"}) {
        anim::SkinnedModel m(default_allocator());
        char error[256] = "";
        CHECK(anim::import_skinned((data + "/" + name).c_str(), 1.0f, m, error, sizeof error));
        std::printf("  %s: %zu morph targets, %zu offsets, %zu material sections\n", name, m.morphs.size(), m.morph_deltas.size(), m.sections.size());
        // Material sections (slots) tile the index list in order, each named once.
        u32 next = 0;
        for (usize s = 0; s < m.sections.size(); ++s) {
            CHECK(m.sections[s].first_index == next);
            next += m.sections[s].index_count;
            for (usize t = 0; t < s; ++t) CHECK(std::strcmp(m.sections[t].material, m.sections[s].material) != 0);
        }
        CHECK(!m.sections.empty() && next == m.indices.size());
        for (const anim::MorphTarget& t : m.morphs) {
            CHECK(t.name[0] && t.count > 0 && t.first + t.count <= m.morph_deltas.size());
            for (u32 i = t.first; i < t.first + t.count; ++i) CHECK(m.morph_deltas[i].vertex < m.vertices.size());
        }
        found += static_cast<u32>(m.morphs.size());
    }
    CHECK(found > 0);

    // Skinned LODs: fewer triangles, every index one of the mesh's own vertices (skinning and morphs unchanged).
    {
        anim::SkinnedModel big(default_allocator());
        char err[256] = "";
        CHECK(anim::import_skinned((data + "/blender_293_barbarian_7400_binary.fbx").c_str(), 1.0f, big, err, sizeof err));
        std::vector<u32> lod(big.indices.size());
        const u32 n = anim::simplify_skinned(big, big.indices.data(), static_cast<u32>(big.indices.size()), 0.25f, lod.data());
        std::printf("  skinned LOD: %zu -> %u indices\n", big.indices.size(), n);
        CHECK(n > 0 && n % 3 == 0 && n < big.indices.size() / 2);
        bool inside = true;
        for (u32 i = 0; i < n; ++i) inside = inside && lod[i] < big.vertices.size();
        CHECK(inside);
    }

    // Curves from the file: an FBX user-defined number property animated in the take, matching ufbx's own value.
    {
        anim::SkinnedModel host(default_allocator());
        char err[256] = "";
        CHECK(anim::import_skinned((data + "/blender_293_half_skinned_7400_binary.fbx").c_str(), 1.0f, host, err, sizeof err));
        const std::string props = data + "/maya_user_props_7700_ascii.fbx";
        anim::Clip takes[2];
        const u32 n = anim::import_clips(props.c_str(), host, takes, 2);
        ufbx_scene* scene = ufbx_load_file(props.c_str(), nullptr, nullptr);
        CHECK(n == 1 && scene != nullptr);
        bool compared = false;
        if (n == 1 && scene) {
            const ufbx_node* cube = ufbx_find_node(scene, "pCube1");
            const ufbx_anim_stack* stack = scene->anim_stacks.count ? scene->anim_stacks.data[0] : nullptr;
            for (const char* prop : {"User_Int_Unbounded", "User_Int_Upper", "User_Number", "User_Float"}) {
                f32 ours = 0.0f;
                if (!cube || !stack || !takes[0].curve_value(prop, 0.5f, ours)) continue;
                const f64 at = stack->time_begin + (stack->time_end - stack->time_begin) * 0.5;
                const f32 theirs = static_cast<f32>(ufbx_evaluate_prop(stack->anim, &cube->element, prop, at).value_real);
                CHECK(std::fabs(ours - theirs) < 0.05f * (1.0f + std::fabs(theirs)));
                compared = true;
            }
            f32 none = 0.0f;
            CHECK(!takes[0].curve_value("No_Such_Curve", 0.5f, none));
        }
        CHECK(compared);  // at least one animated user property was read
        if (scene) ufbx_free_scene(scene);
    }

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

// Unreal rigs by bone names; UE4 <-> UE5 spine and neck pairings.
static void rig_tests() {
    using anim::Rig;
    const char* ue4[] = {"root", "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head", "thigh_l", "upperarm_l", "upperarm_twist_01_l"};
    const char* manny[] = {"root", "pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head", "thigh_l", "upperarm_l", "upperarm_twist_01_l"};
    const char* simple[] = {"root", "pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head", "thigh_l", "upperarm_l", "cc_base_l_eye"};
    const char* other[] = {"Hips", "Spine", "Head", "LeftUpLeg"};
    CHECK(anim::detect_rig(ue4, 10) == Rig::UE4Mannequin);
    CHECK(anim::detect_rig(manny, 13) == Rig::UE5Manny);
    CHECK(anim::detect_rig(simple, 13) == Rig::UE5MannySimple);  // a Character Creator export with UE5 names, no twist bones
    CHECK(anim::detect_rig(other, 4) == Rig::Other);
    CHECK(std::strcmp(anim::rig_source_bone(Rig::UE5Manny, Rig::UE4Mannequin, "spine_05"), "spine_03") == 0);
    CHECK(std::strcmp(anim::rig_source_bone(Rig::UE5MannySimple, Rig::UE4Mannequin, "neck_02"), "neck_01") == 0);
    CHECK(std::strcmp(anim::rig_source_bone(Rig::UE4Mannequin, Rig::UE5Manny, "spine_03"), "spine_05") == 0);
    CHECK(std::strcmp(anim::rig_source_bone(Rig::UE5Manny, Rig::UE5MannySimple, "spine_05"), "spine_05") == 0);  // same family: by name
    CHECK(std::strcmp(anim::rig_source_bone(Rig::UE5Manny, Rig::UE4Mannequin, "hand_l"), "hand_l") == 0);
    CHECK(std::strcmp(anim::rig_name(Rig::UE5MannySimple), "UE5 Manny (Simple)") == 0);
}

// Motion matching: the path a held direction predicts, a database from a real rig's takes, a frame's
// own features finding it again, and playback that keeps returning a clip.
// Ragdolls: a capsule per big bone of a humanoid, parents first, every body but the root hanging from one; a pose set
// from model-space joints skins like the sampled one.
// Full-body IK: a hand target beyond the arm's reach gets closer with the body helping than with the arm alone.
static void full_body_ik_tests(const std::string& data) {
    anim::SkinnedModel model(default_allocator());
    char error[256] = "";
    CHECK(anim::import_skinned((data + "/maya_kenney_character_7700_binary.fbx").c_str(), 1.0f, model, error, sizeof error));
    const i32 hand = model.find_joint("LeftHand");
    CHECK(hand >= 0);
    if (hand < 0) return;
    const anim::BlendInput still{nullptr, 0.0f, 1.0f};
    anim::Pose arm, body;
    arm.sample_blend(model, &still, 1);
    body.sample_blend(model, &still, 1);
    const Mat4 h = arm.joint_model(static_cast<u32>(hand));
    const Vec3 target = Vec3{h.at(0, 3), h.at(1, 3), h.at(2, 3)} + Vec3{0.0f, 0.0f, 1.5f};  // well above the hand
    CHECK(arm.two_bone_ik(model, static_cast<u32>(hand), target, 1.0f));
    const anim::IkGoal goal{static_cast<u32>(hand), target, 1.0f};
    CHECK(anim::full_body_ik(body, model, &goal, 1));
    auto miss = [&](const anim::Pose& p) {
        const Mat4 m = p.joint_model(static_cast<u32>(hand));
        return length(Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - target);
    };
    std::printf("  full-body IK: arm alone misses by %.3f, with the body %.3f\n", static_cast<double>(miss(arm)), static_cast<double>(miss(body)));
    CHECK(miss(body) < miss(arm) * 0.8f);
}

// Retargeting between humanoids named their own ways: the part each bone plays.
static void human_bone_tests() {
    using H = anim::HumanBone;
    struct Case { const char* name; H part; };
    const Case cases[] = {{"pelvis", H::Pelvis}, {"Hips", H::Pelvis}, {"mixamorig:Hips", H::Pelvis}, {"thigh_l", H::ThighL}, {"mixamorig:LeftUpLeg", H::ThighL},
                          {"DEF-thigh.R", H::ThighR}, {"CC_Base_L_Thigh", H::ThighL}, {"LeftLeg", H::CalfL}, {"calf_r", H::CalfR}, {"LeftFoot", H::FootL},
                          {"LeftToeBase", H::ToeL}, {"LeftArm", H::UpperArmL}, {"upperarm_r", H::UpperArmR}, {"LeftForeArm", H::LowerArmL},
                          {"lowerarm_l", H::LowerArmL}, {"RightHand", H::HandR}, {"clavicle_l", H::ClavicleL}, {"LeftShoulder", H::ClavicleL},
                          {"neck_01", H::Neck}, {"Head", H::Head}, {"HeadTop_End", H::None}, {"LeftHandIndex1", H::None}, {"upperarm_twist_01_l", H::None},
                          {"Armature", H::None}, {"spine_02", H::Spine}};
    for (const Case& c : cases) {
        const bool ok = anim::human_bone(c.name) == c.part;
        if (!ok) std::printf("  human_bone(%s) = %d\n", c.name, static_cast<int>(anim::human_bone(c.name)));
        CHECK(ok);
    }
    const char* spine[] = {"Hips", "Spine", "Spine1", "Spine2", "Neck"};
    anim::HumanBone parts[5];
    anim::human_bones(spine, 5, parts);
    CHECK(parts[1] == H::Spine && parts[2] == H::Chest && parts[3] == H::UpperChest && parts[4] == H::Neck);
}

static void ragdoll_tests(const std::string& data) {
    anim::SkinnedModel model(default_allocator());
    char error[256] = "";
    CHECK(anim::import_skinned((data + "/maya_kenney_character_7700_binary.fbx").c_str(), 1.0f, model, error, sizeof error));
    anim::RagdollBody bodies[anim::kMaxRagdollBodies];
    const u32 count = anim::build_ragdoll(model, bodies, anim::kMaxRagdollBodies, 70.0f);
    std::printf("  ragdoll: %u bodies\n", count);
    CHECK(count >= 8);
    u32 roots = 0;
    f32 mass = 0.0f;
    bool foot = false, hips = false;
    for (u32 i = 0; i < count; ++i) {
        const anim::RagdollBody& b = bodies[i];
        CHECK(b.parent < static_cast<i32>(i) && b.radius > 0.005f && b.radius < 0.5f && std::fabs(length(b.axis) - 1.0f) < 1e-3f);
        roots += b.parent < 0 ? 1u : 0u;
        mass += b.mass;
        foot = foot || b.joint == static_cast<u32>(model.find_joint("LeftFoot"));
        hips = hips || b.joint == static_cast<u32>(model.find_joint("Hips"));
    }
    CHECK(roots == 1 && foot && hips && std::fabs(mass - 70.0f) < 1.0f);
    const anim::BlendInput still{nullptr, 0.0f, 1.0f};
    anim::Pose sampled, set;
    sampled.sample_blend(model, &still, 1);
    std::vector<Mat4> models(model.joint_count());
    for (u32 j = 0; j < model.joint_count(); ++j) models[j] = sampled.joint_model(j);
    CHECK(set.set_models(model, models.data()) && set.joint_count() == sampled.joint_count());
    f32 worst = 0.0f;
    for (u32 k = 0; k < sampled.joint_count() * 12; ++k) worst = std::fmax(worst, std::fabs(set.joint_rows()[k] - sampled.joint_rows()[k]));
    CHECK(worst < 1e-4f);
    // Foot placement moves a joint and what hangs from it in model space: the pelvis (all), then one foot.
    const u32 pelvis = static_cast<u32>(model.find_joint("Hips")), foot_l = static_cast<u32>(model.find_joint("LeftFoot")), foot_r = static_cast<u32>(model.find_joint("RightFoot"));
    const f32 before_r = set.joint_model(foot_r).at(2, 3), before_l = set.joint_model(foot_l).at(2, 3), before_hips = set.joint_model(pelvis).at(2, 3);
    CHECK(set.transform_models(model, -1, translation({0.0f, 0.0f, -0.1f})));
    CHECK(std::fabs(set.joint_model(pelvis).at(2, 3) - (before_hips - 0.1f)) < 1e-4f && std::fabs(set.joint_model(foot_r).at(2, 3) - (before_r - 0.1f)) < 1e-4f);
    CHECK(set.transform_models(model, static_cast<i32>(foot_l), translation({0.0f, 0.0f, 0.05f})));
    CHECK(std::fabs(set.joint_model(foot_l).at(2, 3) - (before_l - 0.05f)) < 1e-4f && std::fabs(set.joint_model(foot_r).at(2, 3) - (before_r - 0.1f)) < 1e-4f);
}

static void motion_matching_tests(const std::string& data) {
    anim::MotionInput standing;
    f32 pos[6], face[6];
    anim::predict_trajectory(standing, pos, face);
    for (u32 k = 0; k < 6; ++k) CHECK(std::fabs(pos[k]) < 1e-6f);
    CHECK(std::fabs(face[0] - 1.0f) < 1e-6f && std::fabs(face[1]) < 1e-6f);
    anim::MotionInput walking;  // facing +Y, asked to walk 2 m/s that way: straight ahead in its own space
    walking.facing = walking.desired_facing = 1.5707963f;
    walking.desired_velocity = {0.0f, 2.0f, 0.0f};
    anim::predict_trajectory(walking, pos, face);
    CHECK(pos[0] > 0.1f && pos[2] > pos[0] && pos[4] > pos[2] && pos[4] < 2.0f && std::fabs(pos[5]) < 1e-4f);  // speeding up from rest
    anim::MotionInput turning;
    turning.desired_facing = 1.0f;
    anim::predict_trajectory(turning, pos, face);
    CHECK(face[1] > 0.0f && face[5] > face[1]);  // turning left, further by 1 s

    const std::string path = data + "/maya_kenney_character_7700_binary.fbx";  // skinned, animated, LeftFoot / RightFoot / Hips
    anim::SkinnedModel model(default_allocator());
    char error[256] = "";
    CHECK(anim::import_skinned(path.c_str(), 1.0f, model, error, sizeof error));
    anim::Clip clips[4];
    const u32 count = anim::import_clips(path.c_str(), model, clips, 4);
    const anim::Clip* list[4] = {&clips[0], &clips[1], &clips[2], &clips[3]};
    const bool loops[4] = {true, true, true, true};
    anim::MotionDatabase settings;
    anim::MotionData db(default_allocator());
    const bool built = count > 0 && db.build(model, list, loops, count, settings, 0.0f);
    std::printf("  motion database: %u takes, %zu frames\n", db.clip_count, db.frames.size());
    CHECK(built && db.frames.size() > 10 && db.features.size() == db.frames.size() * anim::kMotionFeatures);
    if (!built) return;
    for (u32 k : {0u, static_cast<u32>(db.frames.size() / 2), static_cast<u32>(db.frames.size() - 1)}) {
        f32 cost = 1.0f;
        const u32 found = anim::search_motion(db, &db.features[k * anim::kMotionFeatures], &cost);
        CHECK(cost < 1e-6f && (found == k || cost == 0.0f));
    }
    // The chunked search finds what looking at every frame finds (queries between frames, not only on them).
    for (u32 k = 0; k + 1 < db.frames.size(); k += 7) {
        f32 q[anim::kMotionFeatures];
        for (u32 d = 0; d < anim::kMotionFeatures; ++d) q[d] = (db.features[k * anim::kMotionFeatures + d] + db.features[(k + 1) * anim::kMotionFeatures + d]) * 0.5f + 0.3f;
        f32 brute = 1e30f, fast = 0.0f;
        for (u32 i = 0; i < db.frames.size(); ++i) {
            f32 d2 = 0.0f;
            for (u32 d = 0; d < anim::kMotionFeatures; ++d) d2 += (db.features[i * anim::kMotionFeatures + d] - q[d]) * (db.features[i * anim::kMotionFeatures + d] - q[d]);
            brute = std::fmin(brute, d2);
        }
        anim::search_motion(db, q, &fast);
        CHECK(std::fabs(fast - brute) < 1e-4f);
    }
    CHECK(!db.chunks.empty() && db.chunks.size() == db.chunk_box.size() / (2 * anim::kMotionFeatures));
    {  // the debugger's candidates: nearest first, the first the search's pick, costs split into pose and path
        anim::MotionCandidate near[5];
        const f32* q = &db.features[5 * anim::kMotionFeatures];
        const u32 n = anim::motion_candidates(db, q, nullptr, near, 5);
        f32 cost = 0.0f;
        const u32 pick = anim::search_motion(db, q, &cost);
        CHECK(n == 5 && std::fabs(near[0].pose_cost + near[0].path_cost - cost) < 1e-5f && (near[0].frame == pick || cost == 0.0f));
        for (u32 k = 1; k < n; ++k) CHECK(near[k - 1].pose_cost + near[k - 1].path_cost <= near[k].pose_cost + near[k].path_cost);
    }
    // Tags: the same take twice, the second tagged "crouch": asked for crouch it is picked, else the untagged one.
    {
        const anim::Clip* twice[2] = {&clips[0], &clips[0]};
        const bool loop2[2] = {true, true};
        anim::MotionDatabase tagged = settings;
        tagged.clips.count = 2;
        std::snprintf(tagged.clips[1].tags, sizeof tagged.clips[1].tags, "%s", "Crouch, slow");
        anim::MotionData db2(default_allocator());
        CHECK(db2.build(model, twice, loop2, 2, tagged, 0.0f));
        const f32* q = &db2.features[3 * anim::kMotionFeatures];
        CHECK(db2.frames[anim::search_motion(db2, q, nullptr, "crouch")].clip == 1);
        CHECK(db2.frames[anim::search_motion(db2, q, nullptr, "")].clip == 0);
        CHECK(db2.frames[anim::search_motion(db2, q, nullptr, "swim")].clip == 0);  // no clip has it: the untagged ones
        CHECK(anim::has_motion_tag("crouch, Slow", "slow") && !anim::has_motion_tag("crouching", "crouch"));
    }
    anim::MotionState state;
    anim::BlendInput inputs[2];
    for (int i = 0; i < 90; ++i) {
        const u32 n = anim::step_motion(db, settings, i < 45 ? standing : walking, state, 1.0f / 30.0f, inputs);
        CHECK(n >= 1 && n <= 2 && inputs[n - 1].clip && inputs[n - 1].ratio >= 0.0f && inputs[n - 1].ratio <= 1.0f);
        f32 total = 0.0f;
        for (u32 k = 0; k < n; ++k) total += inputs[k].weight;
        CHECK(std::fabs(total - 1.0f) < 1e-4f);
    }
    CHECK(settings.blend_mode == 1);  // inertial by default: one input, never a crossfade

    // Leader pose: clothes skinned to the same skeleton copy the body's pose bone by bone, so they skin identically.
    anim::Pose body, shirt;
    body.sample(model, &clips[0], clips[0].duration * 0.5f, true);
    CHECK(shirt.follow(model, model, body) && shirt.joint_count() == body.joint_count());
    f32 worst = 0.0f;
    for (u32 i = 0; i < body.joint_count() * 12; ++i) worst = std::fmax(worst, std::fabs(body.joint_rows()[i] - shirt.joint_rows()[i]));
    CHECK(worst < 1e-4f);
    anim::Pose alone;  // no leader joints yet: nothing to follow
    CHECK(!alone.follow(model, model, anim::Pose{}));
    for (int i = 0; i < 30; ++i) CHECK(anim::step_motion(db, settings, walking, state, 1.0f / 30.0f, inputs) == 1);

    // Inertialization: right after a jump the pose is still the old one (no pop); a few half-lives
    // later it is the new one.
    const anim::Clip* other = count > 1 ? &clips[1] : &clips[0];
    const anim::BlendInput from{&clips[0], 0.25f, 1.0f}, to{other, 0.75f, 1.0f};
    anim::Pose a, b, eased;
    a.sample_blend(model, &from, 1);
    b.sample_blend(model, &to, 1);
    eased.sample_blend(model, &from, 1);
    CHECK(eased.inertialize(model, false, 0.0f, 0.05f));  // remembers the pose it showed
    eased.sample_blend(model, &to, 1);
    CHECK(eased.inertialize(model, true, 0.0f, 0.05f));
    auto distance = [&](const anim::Pose& p, const anim::Pose& q) {
        f32 worst = 0.0f;
        for (u32 k = 0; k < p.joint_count() * 12; ++k) worst = std::fmax(worst, std::fabs(p.joint_rows()[k] - q.joint_rows()[k]));
        return worst;
    };
    const f32 apart = distance(a, b);
    std::printf("  inertial: poses %.3f apart, %.4f from the old one at the jump\n", static_cast<double>(apart), static_cast<double>(distance(eased, a)));
    CHECK(apart > 1e-3f && distance(eased, a) < apart * 0.02f);
    for (int i = 0; i < 30; ++i) {  // 0.5 s: ten half-lives
        eased.sample_blend(model, &to, 1);
        eased.inertialize(model, false, 1.0f / 60.0f, 0.05f);
    }
    CHECK(distance(eased, b) < apart * 0.01f);

    // Mirroring: the mirrored rows are the originals with sideways values negated and the feet
    // swapped; a pose mirrored twice is itself; the mirrored left foot is where the right one was,
    // reflected (the bind pose is the symmetric reference, this rig's rest pose is mid-stride).
    anim::MotionDatabase mirrored_settings;
    mirrored_settings.mirror = true;
    anim::MotionData mdb(default_allocator());
    CHECK(mdb.build(model, list, loops, count, mirrored_settings, 0.0f) && mdb.clip_count == db.clip_count * 2 && mdb.mirrored[1] && !mdb.mirrored[0]);
    if (mdb.clip_count >= 2) {
        const f32* f = &mdb.features[static_cast<usize>(mdb.clip_first[0] + 3) * anim::kMotionFeatures];
        const f32* g = &mdb.features[static_cast<usize>(mdb.clip_first[1] + 3) * anim::kMotionFeatures];
        CHECK(std::fabs(g[1] + f[4]) < 1e-4f && std::fabs(g[0] - f[3]) < 1e-4f && std::fabs(g[16] + f[16]) < 1e-4f && std::fabs(g[22] + f[22]) < 1e-4f);
    }
    const anim::BlendInput still{nullptr, 0.0f, 1.0f};
    anim::Pose rest, rest_mirrored, walk, twice;
    rest.sample_blend(model, &still, 1);
    rest_mirrored.sample_blend(model, &still, 1);
    CHECK(rest_mirrored.mirror(model, 0.0f) && rest_mirrored.mirror(model, 0.0f));
    walk.sample_blend(model, &from, 1);
    twice.sample_blend(model, &from, 1);
    CHECK(twice.mirror(model, 0.0f) && twice.mirror(model, 0.0f));
    const i32 lf = model.find_joint("LeftFoot"), rf = model.find_joint("RightFoot");
    anim::Pose once;
    once.sample_blend(model, &from, 1);
    f32 foot_error = 1e9f;
    if (lf >= 0 && rf >= 0) {
        // Which way the feet sit apart in the bind pose: the sideways axis (this rig faces along Y).
        const Mat4 bl = affine_inverse(model.inverse_bind[static_cast<u32>(lf)]), br = affine_inverse(model.inverse_bind[static_cast<u32>(rf)]);
        const bool sideways_x = std::fabs(bl.at(0, 3) - br.at(0, 3)) > std::fabs(bl.at(1, 3) - br.at(1, 3));
        once.mirror(model, sideways_x ? 1.5707963f : 0.0f);
        const Mat4 l = once.joint_model(static_cast<u32>(lf)), r = walk.joint_model(static_cast<u32>(rf));
        foot_error = sideways_x ? std::fabs(l.at(0, 3) + r.at(0, 3)) + std::fabs(l.at(1, 3) - r.at(1, 3)) + std::fabs(l.at(2, 3) - r.at(2, 3))
                                : std::fabs(l.at(0, 3) - r.at(0, 3)) + std::fabs(l.at(1, 3) + r.at(1, 3)) + std::fabs(l.at(2, 3) - r.at(2, 3));
    }
    std::printf("  mirror: rest twice %.5f, twice %.5f, foot %.4f\n", static_cast<double>(distance(rest, rest_mirrored)), static_cast<double>(distance(walk, twice)), static_cast<double>(foot_error));
    CHECK(distance(rest, rest_mirrored) < 1e-3f && distance(walk, twice) < 1e-3f && foot_error < 0.05f);
}

// A rigged glTF written here: hip (1 m up) -> arm (1 m above it), a triangle on the arm, a take moving the hip 1 m
// along the file's X. Y-up in the file, Z-up in the engine; the bind matrices come from the skin.
static void gltf_skin_tests() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lsd_gltf_skin_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<u8> bin;
    auto put = [&](const void* p, usize n) { bin.insert(bin.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); };
    const f32 positions[9] = {0, 2, 0, 1, 2, 0, 0, 2, 1};
    const u8 joints[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const f32 weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    f32 ibm[32] = {};
    for (int m = 0; m < 2; ++m) {
        for (int i = 0; i < 4; ++i) ibm[m * 16 + i * 5] = 1.0f;
        ibm[m * 16 + 13] = -static_cast<f32>(m + 1);  // inverse of the joint's rest: 1 m and 2 m up
    }
    const f32 times[2] = {0, 1};
    const f32 moves[6] = {0, 1, 0, 1, 1, 0};
    const f32 smile[9] = {0, 0, 0, 0, 0.5f, 0, 0, 0, 0};  // the second corner moves 0.5 m up (file Y) at full weight
    const f32 smile_weights[2] = {0, 1};
    put(positions, sizeof positions);  // 0
    put(joints, sizeof joints);        // 36
    put(weights, sizeof weights);      // 48
    put(ibm, sizeof ibm);              // 96
    put(times, sizeof times);          // 224
    put(moves, sizeof moves);          // 232
    put(smile, sizeof smile);          // 256
    put(smile_weights, sizeof smile_weights);  // 292, 300 in all
    if (FILE* f = std::fopen((dir / "rig.bin").string().c_str(), "wb")) {
        std::fwrite(bin.data(), 1, bin.size(), f);
        std::fclose(f);
    }
    const char* json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2]}],
"nodes":[{"name":"hip","translation":[0,1,0],"children":[1]},{"name":"arm","translation":[0,1,0]},{"name":"body","mesh":0,"skin":0}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"targets":[{"POSITION":6}]}],"weights":[0],"extras":{"targetNames":["Smile"]}}],
"skins":[{"joints":[0,1],"inverseBindMatrices":3}],
"animations":[{"name":"Slide","samplers":[{"input":4,"output":5},{"input":4,"output":7}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},{"sampler":1,"target":{"node":2,"path":"weights"}}]}],
"buffers":[{"uri":"rig.bin","byteLength":300}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12},{"buffer":0,"byteOffset":48,"byteLength":48},
{"buffer":0,"byteOffset":96,"byteLength":128},{"buffer":0,"byteOffset":224,"byteLength":8},{"buffer":0,"byteOffset":232,"byteLength":24},
{"buffer":0,"byteOffset":256,"byteLength":36},{"buffer":0,"byteOffset":292,"byteLength":8}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,2,0],"max":[1,2,1]},{"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":3,"componentType":5126,"count":2,"type":"MAT4"},
{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"},
{"bufferView":6,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":7,"componentType":5126,"count":2,"type":"SCALAR"}]})";
    const std::string path = (dir / "rig.gltf").string();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(json, 1, std::strlen(json), f);
        std::fclose(f);
    }
    auto near = [](Vec3 a, Vec3 b) { return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f && std::fabs(a.z - b.z) < 1e-3f; };
    char error[256] = "";
    {  // morphs without a skeleton: the same triangle and "Smile" on a plain node 1 m up imports with a Root-only skeleton
        const char* face = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"face","mesh":0,"translation":[0,1,0]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"targets":[{"POSITION":1}]}],"weights":[0],"extras":{"targetNames":["Smile"]}}],
"buffers":[{"uri":"rig.bin","byteLength":300}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":256,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,2,0],"max":[1,2,1]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}]})";
        const std::string face_path = (dir / "face.gltf").string();
        if (FILE* f = std::fopen(face_path.c_str(), "wb")) {
            std::fwrite(face, 1, std::strlen(face), f);
            std::fclose(f);
        }
        anim::SkinnedModel rigid(default_allocator());
        CHECK(anim::import_skinned(face_path.c_str(), 1.0f, rigid, error, sizeof error) && rigid.joint_count() == 1 && rigid.morphs.size() == 1);
        if (rigid.vertices.size() == 3) {
            CHECK(near({rigid.vertices[0].position[0], rigid.vertices[0].position[1], rigid.vertices[0].position[2]}, {0, 0, 3}));  // 2 m + the node's 1 m
            anim::Pose still;
            still.sample(rigid, nullptr, 0.0f, false);
            CHECK(near(skin(still, rigid.vertices[0]), {0, 0, 3}));  // on the Root: stays put
        }
    }
    anim::SkinnedModel model(default_allocator());
    CHECK(anim::import_skinned(path.c_str(), 1.0f, model, error, sizeof error));
    CHECK(model.joint_count() == 3 && model.find_joint("Root") >= 0 && model.find_joint("hip") >= 0 && model.find_joint("arm") >= 0);
    CHECK(model.vertices.size() == 3 && model.indices.size() == 3 && model.sections.size() == 1);
    if (model.vertices.size() != 3) return;
    CHECK(near({model.vertices[0].position[0], model.vertices[0].position[1], model.vertices[0].position[2]}, {0, 0, 2}));  // Y-up 2 m -> Z-up
    CHECK(near({model.vertices[2].position[0], model.vertices[2].position[1], model.vertices[2].position[2]}, {0, -1, 2}));  // file +Z -> engine -Y
    CHECK(model.vertices[0].joints[0] == static_cast<u8>(model.find_joint("arm")) && model.vertices[0].weights[0] == 255);
    anim::Pose pose;
    pose.sample(model, nullptr, 0.0f, false);
    CHECK(near(skin(pose, model.vertices[0]), {0, 0, 2}));  // the rest pose leaves it where it is
    anim::Clip clips[2];
    CHECK(anim::import_clips(path.c_str(), model, clips, 2) == 1);
    CHECK(std::strcmp(clips[0].name, "Slide") == 0 && std::fabs(clips[0].duration - 1.0f) < 1e-4f);
    // Morph targets: "Smile" moves the second corner 0.5 m up (Z in the engine); the take raises its weight 0 to 1.
    // Its normals turn too: all three corners of the tilted triangle get a normal offset.
    CHECK(model.morphs.size() == 1 && std::strcmp(model.morphs[0].name, "Smile") == 0 && model.morphs[0].count == 3);
    bool moved = false;
    for (u32 i = model.morphs[0].first; model.morphs.size() == 1 && i < model.morphs[0].first + model.morphs[0].count; ++i) {
        const anim::MorphDelta& d = model.morph_deltas[i];
        if (d.vertex == 1) moved = std::fabs(d.offset[2] - 0.5f) < 1e-5f && std::fabs(d.offset[1]) < 1e-5f;
        else CHECK(d.offset[0] == 0.0f && d.offset[1] == 0.0f && d.offset[2] == 0.0f);
    }
    CHECK(moved);
    {  // at full weight every corner's normal is the morphed triangle's
        std::vector<rhi::SkinnedVertex> shaped(model.vertices.size());
        anim::apply_morphs(model, [](const char*) { return 1.0f; }, shaped.data());
        const Vec3 a{shaped[0].position[0], shaped[0].position[1], shaped[0].position[2]}, b{shaped[1].position[0], shaped[1].position[1], shaped[1].position[2]},
            c{shaped[2].position[0], shaped[2].position[1], shaped[2].position[2]};
        Vec3 face = normalize(cross(b - a, c - a));
        const Vec3 n{shaped[0].normal[0], shaped[0].normal[1], shaped[0].normal[2]};
        if (dot(face, n) < 0.0f) face = face * -1.0f;
        CHECK(dot(face, n) > 0.999f && std::fabs(length(n) - 1.0f) < 1e-4f);
    }
    f32 smile_now = -1.0f;
    CHECK(clips[0].morph_weight("Smile", 0.5f, smile_now) && std::fabs(smile_now - 0.5f) < 0.02f);
    pose.sample(model, &clips[0], 1.0f, false);
    CHECK(near(skin(pose, model.vertices[0]), {1, 0, 2}));  // the hip slid 1 m, the arm and its triangle with it
    pose.sample(model, &clips[0], 0.5f, false);
    CHECK(near(skin(pose, model.vertices[1]), {1.5f, 0, 2}));
    CHECK(model.motion_root() == model.find_joint("hip"));  // root motion: the hip's slide comes out, the mesh stays put
    pose.sample(model, &clips[0], 1.0f, false);
    CHECK(near(pose.strip_root(model), {1, 0, 0}) && near(skin(pose, model.vertices[0]), {0, 0, 2}));
    const anim::ClipTrim trim{0, 0.5f, 0.0f};  // keep the second half: it starts where the hip is half-way
    CHECK(anim::import_clips(path.c_str(), model, clips, 2, &trim, 1) == 1 && std::fabs(clips[0].duration - 0.5f) < 1e-4f);
    pose.sample(model, &clips[0], 0.0f, false);
    CHECK(near(skin(pose, model.vertices[0]), {0.5f, 0, 2}));
    anim::SkinnedModel twice(default_allocator());
    CHECK(anim::import_skinned(path.c_str(), 2.0f, twice, error, sizeof error));
    pose.sample(twice, nullptr, 0.0f, false);
    if (twice.vertices.size() == 3) CHECK(near(skin(pose, twice.vertices[0]), {0, 0, 4}) && twice.bounds_max.z > 3.99f);  // the import scale
    fs::remove_all(dir, ec);
}

int main() {
    rig_tests();
    gltf_skin_tests();
    graph_tests();
    graph_editing_tests();
    const std::string data = LSD_UFBX_DATA_DIR;
    motion_matching_tests(data);
    ragdoll_tests(data);
    human_bone_tests();
    full_body_ik_tests(data);
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
            // Montage layering: the clip laid fully over the rest pose is the clip; from the last joint only, the root stays at rest.
            anim::Pose full, layered, part;
            full.sample(model, &clips[0], clips[0].duration * 0.5f, false);
            layered.sample(model, nullptr, 0.0f, false);
            part.sample(model, nullptr, 0.0f, false);
            const Mat4 root_rest = part.joint_model(0);
            CHECK(layered.layer(model, &clips[0], 0.5f, 1.0f, -1));
            for (u32 j = 0; j < model.joint_count(); ++j) {
                const Mat4 a = full.joint_model(j), b = layered.joint_model(j);
                CHECK(length(Vec3{a.at(0, 3) - b.at(0, 3), a.at(1, 3) - b.at(1, 3), a.at(2, 3) - b.at(2, 3)}) < size * 1e-3f);
            }
            const i32 last = static_cast<i32>(model.joint_count()) - 1;
            CHECK(part.layer(model, &clips[0], 0.5f, 1.0f, last));
            const Mat4 root_now = part.joint_model(0);
            CHECK(last == 0 || length(Vec3{root_now.at(0, 3) - root_rest.at(0, 3), root_now.at(1, 3) - root_rest.at(1, 3), root_now.at(2, 3) - root_rest.at(2, 3)}) < 1e-5f);
            CHECK(!part.layer(model, &clips[0], 0.5f, 0.0f, -1));  // weight 0 does nothing
            // Root motion: what strip_root returns is the root's sideways move from rest, and the root is back at rest sideways.
            const u32 mr = static_cast<u32>(model.motion_root());
            CHECK(mr > 0);  // never the import's own Root
            anim::Pose at_rest;
            at_rest.sample(model, nullptr, 0.0f, false);
            const Mat4 root_rest_mr = at_rest.joint_model(mr);
            const Mat4 root_before = full.joint_model(mr);
            const Vec3 moved = full.strip_root(model);
            const Mat4 root_after = full.joint_model(mr);
            CHECK(std::fabs(moved.x - (root_before.at(0, 3) - root_rest_mr.at(0, 3))) < size * 1e-3f && std::fabs(moved.y - (root_before.at(1, 3) - root_rest_mr.at(1, 3))) < size * 1e-3f);
            CHECK(std::fabs(root_after.at(0, 3) - root_rest_mr.at(0, 3)) < size * 1e-3f && std::fabs(root_after.at(1, 3) - root_rest_mr.at(1, 3)) < size * 1e-3f);
            CHECK(std::fabs(root_after.at(2, 3) - root_before.at(2, 3)) < 1e-5f);  // height is the animation's
            ++tested_animation;
        }
    }
    CHECK(tested_animation > 0);  // at least one file must exercise animation
    if (g_failures == 0) std::printf("All animation tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
