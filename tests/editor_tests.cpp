// Editor tests that run without a GPU: undo / redo history and unsaved-change tracking.
#include "assistant_core.h"
#include "hlod.h"
#include "dynamic_gi.h"
#include "light_bake.h"
#include "lsd/asset/texture.h"
#include "lighting.h"
#include "lsd/render/mesh.h"
#include "references.h"

#include "lsd/asset/asset_registry.h"

#include <filesystem>
#include <fstream>
#include "keymap.h"
#include "landscape_paint.h"
#include "rml_edit.h"
#include "editor_settings.h"
#include "sound_cue.h"
#include "transactions.h"
#include "behavior.h"
#include "landscape_core.h"
#include "mesh_edit.h"
#include "loc_gather.h"
#include "scalability.h"
#include "template_levels.h"
#include "paper2d_core.h"
#include "pcg_graph.h"
#include "cloth_core.h"
#include "editor_utility_core.h"
#include "multi_user_core.h"
#include "asset_compiler.h"
#include "lsd/world/level.h"
#include "lsd/ui/ui_sounds.h"

#include "lsd/world/components.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

static void test_undo_redo() {
    HeapAllocator heap("editor");
    {
        world::World w(heap);
        editor::Transactions history(heap);
        const world::Entity box = w.create("Box");
        history.reset(w, "Open Level");
        CHECK(!history.dirty() && !history.can_undo());

        w.get<world::Transform>(box)->position = {3, 0, 0};
        history.commit(w, "Move");
        const world::Entity ball = w.create("Ball");
        history.commit(w, "Place Ball");
        CHECK(history.dirty() && history.count() == 3 && std::strcmp(history.label(2), "Place Ball") == 0);

        CHECK(history.undo(w));
        CHECK(!w.alive(ball) && w.alive(box) && w.get<world::Transform>(box)->position.x == 3.0f);
        CHECK(history.undo(w));
        CHECK(w.get<world::Transform>(box)->position.x == 0.0f && !history.dirty());  // back at the saved state
        CHECK(!history.undo(w));
        CHECK(history.redo(w) && history.redo(w) && w.alive(ball));  // same handle comes back
        CHECK(!history.redo(w));

        history.mark_saved();
        history.undo(w);
        w.destroy(box);
        history.commit(w, "Delete Box");  // drops the redo branch that held the saved state
        CHECK(!history.can_redo() && history.dirty() && !w.alive(box));
        CHECK(history.jump_to(w, 0) && w.alive(box) && !w.alive(ball));
    }
    CHECK(heap.allocated_bytes() == 0);
}

// Sound Cues: Random never repeats, Modulator stays in range, Concatenator queues by length, Switch
// follows the parameter, Output scales everything.
static void test_sound_cue() {
    using namespace lsd::script;
    using namespace lsd::editor;
    Graph g(default_allocator());
    const u32 output = g.add("CueOutput", 0, 0)->id;
    std::snprintf(g.find(output)->values[1], 64, "0.5");  // overall volume
    const u32 mixer = g.add("CueMixer", 0, 0)->id;
    const u32 random = g.add("CueRandom", 0, 0)->id;
    const u32 modulator = g.add("CueModulator", 0, 0)->id;
    const u32 concat = g.add("CueConcat", 0, 0)->id;
    const u32 swtch = g.add("CueSwitch", 0, 0)->id;
    u32 waves[6];
    const char* names[6] = {"Step1", "Step2", "Step3", "Intro", "Body", "Grass"};
    for (u32 i = 0; i < 6; ++i) {
        waves[i] = g.add("CueWave", 0, 0)->id;
        std::snprintf(g.find(waves[i])->values[0], 64, "%s", names[i]);
    }
    CHECK(g.connect(mixer, 0, output, 0));
    CHECK(g.connect(waves[0], 0, random, 0) && g.connect(waves[1], 0, random, 1) && g.connect(waves[2], 0, random, 2));
    CHECK(g.connect(random, 0, modulator, 0) && g.connect(modulator, 0, mixer, 0));
    CHECK(g.connect(waves[3], 0, concat, 0) && g.connect(waves[4], 0, concat, 1) && g.connect(concat, 0, mixer, 1));
    CHECK(g.connect(waves[5], 0, swtch, 2) && g.connect(swtch, 0, mixer, 2));
    CHECK(!g.connect(waves[0], 0, g.find(waves[1])->id, 0));  // Audio into a sound name: refused

    CueEnv env;
    env.length = [](void*, const char* sound) { return std::strcmp(sound, "Intro") == 0 ? 1.5f : 2.0f; };
    static f32 surface = 0.0f;
    env.parameter = [](void*, const char*) { return surface; };
    CueVoice v[16];
    char previous[64] = "";
    for (int play = 0; play < 20; ++play) {
        surface = play % 2 ? 2.0f : 0.0f;  // grass on odd plays
        const u32 n = evaluate_cue(g, env, v, 16);
        CHECK(n == (play % 2 ? 4u : 3u));
        CHECK(std::strncmp(v[0].sound, "Step", 4) == 0 && std::strcmp(v[0].sound, previous) != 0);  // no repeat
        std::snprintf(previous, sizeof previous, "%s", v[0].sound);
        CHECK(v[0].volume >= 0.45f - 1e-4f && v[0].volume <= 0.55f + 1e-4f && v[0].pitch >= 0.95f && v[0].pitch <= 1.05f);  // 0.5 x [0.9, 1.1]
        CHECK(std::strcmp(v[1].sound, "Intro") == 0 && v[1].delay == 0.0f && std::strcmp(v[2].sound, "Body") == 0 && v[2].delay == 1.5f);
        CHECK(v[1].volume == 0.5f);
        if (play % 2) CHECK(std::strcmp(v[3].sound, "Grass") == 0);
    }
    // Delay and Looping wrap what comes in.
    Graph h(default_allocator());
    const u32 out2 = h.add("CueOutput", 0, 0)->id;
    const u32 loop = h.add("CueLooping", 0, 0)->id;
    const u32 delay = h.add("CueDelay", 0, 0)->id;
    const u32 wave = h.add("CueWave", 0, 0)->id;
    std::snprintf(h.find(wave)->values[0], 64, "Rain");
    std::snprintf(h.find(delay)->values[1], 64, "1");
    std::snprintf(h.find(delay)->values[2], 64, "1");
    CHECK(h.connect(wave, 0, delay, 0) && h.connect(delay, 0, loop, 0) && h.connect(loop, 0, out2, 0));
    CHECK(evaluate_cue(h, env, v, 16) == 1 && v[0].looping && v[0].delay == 1.0f);
}

// Sound Synth graphs compile to a program in dependency order; loops and foreign nodes are refused.
static void test_sound_synth() {
    using namespace lsd::editor;
    using lsd::script::Graph;
    Graph g(default_allocator());
    const u32 output = g.add("SynOutput", 0, 0)->id;
    const u32 gain = g.add("SynGain", 0, 0)->id;
    const u32 osc = g.add("SynOscillator", 0, 0)->id;
    const u32 pitch = g.add("SynParam", 0, 0)->id;
    const u32 env = g.add("SynEnvelope", 0, 0)->id;
    const u32 hit = g.add("SynTrigger", 0, 0)->id;
    const u32 wave = g.add("SynWave", 0, 0)->id;
    const u32 mix = g.add("SynMix", 0, 0)->id;
    std::snprintf(g.find(osc)->values[2], 64, "Square");
    std::snprintf(g.find(wave)->values[0], 64, "Thud");
    CHECK(g.connect(pitch, 0, osc, 0) && g.connect(osc, 0, gain, 0) && g.connect(hit, 0, env, 0) && g.connect(env, 0, gain, 1));
    CHECK(g.connect(gain, 0, mix, 0) && g.connect(hit, 0, wave, 2) && g.connect(wave, 0, mix, 1) && g.connect(mix, 0, output, 0));
    auto* p = new lsd::audio::SynthProgram();
    char problem[160];
    CHECK(compile_synth(g, *p, problem, sizeof problem));
    CHECK(p->count == 7 && p->param_count == 2 && p->wave_count == 1 && std::strcmp(p->waves[0], "Thud") == 0);
    CHECK(std::strcmp(p->params[0], "Pitch") == 0 && p->param_defaults[0] == 440.0f && std::strcmp(p->params[1], "Hit") == 0);
    for (u32 i = 0; i < p->count; ++i) {  // every wired input reads an earlier node
        for (u32 k = 0; k < lsd::audio::kSynthInputs; ++k) CHECK(p->nodes[i].in[k].node < static_cast<i32>(i));
    }
    CHECK(p->output.node == static_cast<i32>(p->count) - 1 && p->nodes[p->count - 1].op == lsd::audio::SynthOp::Mix);
    bool square = false, played = false;
    for (u32 i = 0; i < p->count; ++i) {
        square |= p->nodes[i].op == lsd::audio::SynthOp::Oscillator && p->nodes[i].mode == static_cast<u8>(lsd::audio::SynthShape::Square);
        played |= p->nodes[i].op == lsd::audio::SynthOp::Wave && p->nodes[i].b == 1.0f;  // Play is wired
    }
    CHECK(square && played);
    CHECK(g.connect(mix, 0, osc, 1));  // a loop: refused by the compiler
    CHECK(!compile_synth(g, *p, problem, sizeof problem) && std::strstr(problem, "loop"));
    g.disconnect_input(osc, 1);
    g.add("CueWave", 0, 0);
    CHECK(g.connect(g.nodes[g.nodes.size() - 1].id, 0, mix, 2) == false);  // a Sound Cue node does not wire into a synth
    Graph empty(default_allocator());
    CHECK(!compile_synth(empty, *p, problem, sizeof problem));
    delete p;
}

static void test_rml_anchor() {  // UMG anchors: re-anchoring keeps the element where it is
    using namespace lsd::editor;
    std::string rml = "<rml><body><div id=\"hud\" style=\"position: absolute; left: 100px; top: 50px; width: 200px; height: 40px;\"></div></body></rml>";
    CHECK(rml_anchor(rml, "hud").x == 0 && rml_anchor(rml, "hud").y == 0);
    const float box[4] = {100, 50, 200, 40};
    CHECK(rml_set_anchor(rml, "hud", RmlAnchor{2, 2}, box, 1280, 720));  // bottom right
    CHECK(rml_style(rml, "hud", "right") == "980px" && rml_style(rml, "hud", "bottom") == "630px" && rml_style(rml, "hud", "left").empty());
    CHECK(rml_anchor(rml, "hud").x == 2 && rml_anchor(rml, "hud").y == 2);
    CHECK(rml_set_anchor(rml, "hud", RmlAnchor{1, 0}, box, 1280, 720));  // top centre
    CHECK(rml_style(rml, "hud", "left") == "50%" && rml_style(rml, "hud", "margin-left") == "-540px" && rml_style(rml, "hud", "right").empty() &&
          rml_style(rml, "hud", "top") == "50px" && rml_style(rml, "hud", "bottom").empty());
    CHECK(rml_anchor(rml, "hud").x == 1 && rml_anchor(rml, "hud").y == 0);
}

static void test_rml_edit() {  // the UI designer's writes into the document text
    using namespace lsd::editor;
    std::string rml = "<rml><body>\n<div id=\"panel\" style=\"left: 40px; color: red;\">\n  <div id=\"title\">Title</div>\n  <button id=\"play\">Play</button>\n</div>\n<img id=\"pic\" src=\"a.png\"/>\n</body></rml>";
    {  // the Hierarchy: nesting from the tags, self-closing ones have no children, comments are skipped
        const std::vector<RmlNode> tree = rml_outline("<rml><head><title>x</title></head><body><!-- <div id=\"no\"> -->" + rml.substr(rml.find("<body>") + 6));
        CHECK(tree.size() == 4 && tree[0].id == "panel" && tree[0].depth == 0 && tree[1].id == "title" && tree[1].depth == 1);
        CHECK(tree[2].tag == "button" && tree[2].depth == 1 && tree[3].id == "pic" && tree[3].tag == "img" && tree[3].depth == 0);
    }
    CHECK(rml_style(rml, "panel", "left") == "40px" && rml_style(rml, "panel", "top").empty());
    CHECK(rml_set_style(rml, "panel", "left", "120px") && rml_style(rml, "panel", "left") == "120px");
    CHECK(rml_set_style(rml, "panel", "top", "8px") && rml_style(rml, "panel", "top") == "8px" && rml_style(rml, "panel", "color") == "red");
    CHECK(rml_set_style(rml, "panel", "color", "") && rml_style(rml, "panel", "color").empty());
    CHECK(rml_set_style(rml, "title", "font-size", "30px") && rml_attribute(rml, "title", "style") == "font-size: 30px;");
    CHECK(rml_set_style(rml, "pic", "width", "64px") && rml.find("<img id=\"pic\" src=\"a.png\" style=\"width: 64px;\"/>") != std::string::npos);
    std::string text;
    CHECK(rml_text(rml, "title", text) && text == "Title");
    CHECK(!rml_text(rml, "panel", text));  // has child elements
    CHECK(rml_set_text(rml, "play", "Start") && rml_text(rml, "play", text) && text == "Start");
    CHECK(rml_set_attribute(rml, "play", "onclick", "start_game") && rml_attribute(rml, "play", "onclick") == "start_game");
    CHECK(rml_unique_id(rml, "button") == "button1");
    CHECK(rml_append_to_body(rml, "<div id=\"text1\">Hi</div>") && rml_text(rml, "text1", text) && text == "Hi");
    CHECK(rml_remove(rml, "panel") && rml.find("title") == std::string::npos && rml.find("pic") != std::string::npos && rml.find("text1") != std::string::npos);
    size_t b, e;
    CHECK(!rml_find_tag(rml, "nope", b, e));
}

static void test_light_bake() {  // Build Lighting: open sky over a black floor
    using namespace lsd;
    editor::BakeScene scene;
    const Vec3 a{-100, -100, 0}, b{100, -100, 0}, c{100, 100, 0}, d{-100, 100, 0};
    scene.positions = {a, b, c, a, c, d};
    scene.albedo = {Vec3{}, Vec3{}};
    for (int i = 0; i < 3; ++i) scene.lights.sky[i] = scene.lights.ground[i] = 1.0f;
    scene.lights.count = 0;
    scene.lo = {-1, -1, 1};
    scene.hi = {1, 1, 2};
    scene.spacing = 1.0f;
    editor::BakedLighting baked;
    editor::bake_lighting(scene, baked);
    CHECK(baked.probe_count() == 3 * 3 * 2 && baked.sh.size() == baked.probe_count() * 12);
    auto ambient = [&](u32 probe, f32 nz) {  // what the shader computes for a red normal pointing up / down
        const f32* r = &baked.sh[probe * 12];
        return (0.886227f * r[0] + 1.023328f * r[3] * nz) / 3.14159265f;
    };
    CHECK(ambient(0, 1.0f) > 0.95f && ambient(0, 1.0f) < 1.05f);  // the whole sky above
    CHECK(ambient(0, -1.0f) > -0.05f && ambient(0, -1.0f) < 0.05f);  // the black floor below
    rhi::LightSetup setup;
    editor::apply_baked_lighting(baked, setup);
    CHECK(setup.probe_origin[3] == 1.0f && setup.probe_dims[0] == 3 && setup.probes[0][0] == baked.sh[0]);
}

static void test_dynamic_gi() {  // Dynamic GI: the grid follows the camera, keeps what it already lit, relights the sky
    using namespace lsd;
    const u32 dims[3] = {4, 4, 2};
    i32 origin[3];
    editor::gi_grid_origin(Vec3{5.1f, -0.5f, 1.0f}, 2.0f, dims, origin);
    CHECK(origin[0] == 0 && origin[1] == -3 && origin[2] == -1);  // floor(eye / cell) - dims / 2
    std::vector<f32> in(4 * 4 * 2 * 12), out(in.size());
    for (usize i = 0; i < in.size(); ++i) in[i] = static_cast<f32>(i / 12);  // each probe holds its own index
    std::vector<u8> kept(4 * 4 * 2);
    const i32 from[3] = {0, 0, 0}, to[3] = {1, 0, 0};  // moved one cell along +x
    editor::gi_scroll(from, to, dims, in.data(), out.data(), kept.data());
    CHECK(kept[0] && out[0] == 1.0f && out[12 * 2] == 3.0f);  // new probe 0 is old probe 1
    CHECK(!kept[3] && out[3 * 12] == 0.0f && !kept[7]);       // the last column is new
    CHECK(kept[16] && out[16 * 12] == 17.0f);                 // the upper layer moves the same way
    editor::BakeScene scene;  // a black floor under a white sky, traced as dynamic GI does
    const Vec3 a{-100, -100, 0}, b{100, -100, 0}, c{100, 100, 0}, d{-100, 100, 0};
    scene.positions = {a, b, c, a, c, d};
    scene.albedo = {Vec3{}, Vec3{}};
    rhi::LightSetup lights;
    for (int i = 0; i < 3; ++i) lights.sky[i] = lights.ground[i] = 1.0f;
    lights.count = 0;
    auto tracer = editor::make_probe_tracer(std::move(scene));
    editor::set_tracer_lights(*tracer, lights, 0);
    const f32 at[3] = {0, 0, 1}, cell[3] = {1, 1, 1};
    std::vector<f32> sh(4 * 4 * 2 * 12);
    editor::relight_probe_grid(*tracer, at, cell, dims, 64, 7, 2, nullptr, sh.data());
    const f32 up = (0.886227f * sh[0] + 1.023328f * sh[3]) / 3.14159265f, down = (0.886227f * sh[0] - 1.023328f * sh[3]) / 3.14159265f;
    CHECK(up > 0.9f && up < 1.1f && down > -0.1f && down < 0.1f);
}

static void test_static_light_bake() {  // Mobility Static: a point light's direct light ends up in the probes (only when Static)
    using namespace lsd;
    editor::BakeScene scene;
    const Vec3 a{-10, -10, 0}, b{10, -10, 0}, c{10, 10, 0}, d{-10, 10, 0};
    scene.positions = {a, b, c, a, c, d};
    scene.albedo = {Vec3{}, Vec3{}};  // a black floor: no bounce to confuse it
    for (int i = 0; i < 3; ++i) scene.lights.sky[i] = scene.lights.ground[i] = 0.0f;
    scene.lights.count = 1;
    scene.lights.lights[0] = rhi::GpuLight{};
    scene.lights.lights[0].type = 1.0f;
    scene.lights.lights[0].position[2] = 3.0f;
    scene.lights.lights[0].range = 10.0f;
    for (int i = 0; i < 3; ++i) scene.lights.lights[0].color[i] = 10.0f;
    scene.lo = {-1, -1, 1};
    scene.hi = {1, 1, 2};
    scene.spacing = 1.0f;
    editor::BakedLighting movable, baked;
    editor::bake_lighting(scene, movable);
    scene.static_lights = 1;
    editor::bake_lighting(scene, baked);
    CHECK(!movable.sh.empty() && !baked.sh.empty() && std::fabs(movable.sh[0]) < 1e-4f && baked.sh[0] > 0.1f);
    CHECK(baked.sh[3] > 0.0f);  // red L1 along +Z: it comes from above
}

static void test_lightmaps() {  // Build Lighting's lightmaps: a 20 m floor under a 4 m roof, the sun straight down
    using namespace lsd;
    editor::BakeScene scene;
    const Vec3 a{-10, -10, 0}, b{10, -10, 0}, c{10, 10, 0}, d{-10, 10, 0};
    const Vec3 ra{-2, -2, 2}, rb{2, -2, 2}, rc{2, 2, 2}, rd{-2, 2, 2};
    scene.positions = {a, b, c, a, c, d, ra, rb, rc, ra, rc, rd};
    scene.albedo = {Vec3{0.5f, 0.5f, 0.5f}, Vec3{0.5f, 0.5f, 0.5f}, Vec3{}, Vec3{}};
    for (int i = 0; i < 3; ++i) scene.lights.sky[i] = scene.lights.ground[i] = 1.0f;
    scene.lights.count = 1;
    scene.lights.lights[0] = rhi::GpuLight{};
    scene.lights.lights[0].type = 0.0f;
    scene.lights.lights[0].direction[2] = -1.0f;
    for (int i = 0; i < 3; ++i) scene.lights.lights[0].color[i] = 3.0f;
    scene.lo = {-1, -1, 1};
    scene.hi = {1, 1, 1.5f};
    scene.spacing = 1.0f;
    scene.lightmap_size = 256;
    editor::BakeScene::LightmapSource floor;
    std::snprintf(floor.object.name, sizeof floor.object.name, "Floor");
    std::snprintf(floor.object.mesh, sizeof floor.object.mesh, "Plane");
    render::MeshData plane = render::make_plane(default_allocator());
    floor.object.vertices.assign(plane.vertices, plane.vertices + plane.vertex_count);
    floor.object.indices.assign(plane.indices, plane.indices + plane.index_count);
    render::free_mesh(default_allocator(), plane);
    floor.world = scaling(Vec3{20, 20, 1});
    scene.lightmap_sources.push_back(floor);
    editor::BakedLighting baked;
    std::atomic<u32> done{0}, total{0};
    editor::bake_lighting(scene, baked, &done, &total);
    CHECK(baked.has_lightmap() && baked.objects.size() == 1 && baked.lightmap.size() == 256u * 256u * 4u && done.load() == total.load());
    bool uvs_in_atlas = !baked.objects.empty();
    for (const rhi::MeshVertex& v : baked.objects.empty() ? std::vector<rhi::MeshVertex>{} : baked.objects[0].vertices) {
        uvs_in_atlas = uvs_in_atlas && v.lightmap_uv[0] > 0.0f && v.lightmap_uv[0] < 1.0f && v.lightmap_uv[1] > 0.0f && v.lightmap_uv[1] < 1.0f;
    }
    CHECK(uvs_in_atlas);
    u32 covered = 0, shadowed = 0;
    u32 lit_sum = 0, dark_sum = 0;
    for (usize i = 0; i < baked.lightmap.size(); i += 4) {
        if (!baked.lightmap[i]) continue;  // outside the charts
        ++covered;
        if (baked.lightmap[i + 3] < 128) {
            ++shadowed;
            dark_sum += baked.lightmap[i];
        } else {
            lit_sum += baked.lightmap[i];
        }
    }
    // Directional: the light comes mostly from above in the open (the sky), so the stored directions point up.
    CHECK(baked.lightmap_dir.size() == baked.lightmap.size());
    f64 up = 0.0;
    for (usize i = 0; i < baked.lightmap.size(); i += 4) {
        if (baked.lightmap[i] && baked.lightmap[i + 3] >= 128) up += (baked.lightmap_dir[i + 2] / 255.0) * 2.0 - 1.0;
    }
    CHECK(covered > shadowed && up / static_cast<f64>(covered - shadowed) > 0.4);
    {  // a Stationary point light above the roof: its shadow mask is dark in the roof's shadow, about 12 m across
        editor::BakeScene lamp = scene;
        lamp.lights.count = 2;
        lamp.lights.lights[1] = rhi::GpuLight{};
        lamp.lights.lights[1].type = 1.0f;
        lamp.lights.lights[1].position[2] = 3.0f;
        lamp.lights.lights[1].range = 40.0f;
        lamp.static_lights = 1u << (16 + 1);
        editor::BakedLighting masked;
        editor::bake_lighting(lamp, masked);
        CHECK(masked.mask_count == 1 && masked.shadow_mask.size() == masked.lightmap.size() && std::fabs(masked.mask_lights[0][2] - 3.0f) < 1e-4f);
        u32 dark = 0, cells = 0;
        for (usize i = 0; i < masked.lightmap.size() && masked.shadow_mask.size() == masked.lightmap.size(); i += 4) {
            if (!masked.lightmap[i]) continue;
            ++cells;
            dark += masked.shadow_mask[i] < 128 ? 1u : 0u;
        }
        const f32 masked_share = cells ? static_cast<f32>(dark) / static_cast<f32>(cells) : 0.0f;
        CHECK(masked_share > 0.28f && masked_share < 0.44f);  // 12 x 12 of 20 x 20 m
    }
    const f32 share = covered ? static_cast<f32>(shadowed) / static_cast<f32>(covered) : 0.0f;
    CHECK(covered > 1000 && share > 0.02f && share < 0.08f);  // 16 of 400 square metres under the roof
    CHECK(shadowed && covered > shadowed && dark_sum / shadowed < lit_sum / (covered - shadowed));  // and less sky there
    CHECK(editor::find_lightmap_object(baked, "Floor", "Plane", {0, 0, 0}) == 0 && editor::find_lightmap_object(baked, "Floor", "Plane", {1, 0, 0}) == -1 &&
          editor::find_lightmap_object(baked, "Floor", "Cube", {0, 0, 0}) == -1);
    rhi::LightSetup setup;
    editor::apply_baked_lighting(baked, setup);
    CHECK(setup.lightmap_info[1] == 1.0f && setup.lightmap_info[0] == baked.lightmap_scale);
    const std::string path = (std::filesystem::path(LSD_TEST_TEMP_DIR) / "Lightmap_BuiltData.lsdlight").generic_string();
    CHECK(editor::save_baked_lighting(path.c_str(), baked));
    editor::BakedLighting loaded;
    CHECK(editor::load_baked_lighting(path.c_str(), loaded) && loaded.has_lightmap() && loaded.lightmap == baked.lightmap && loaded.objects.size() == 1 &&
          loaded.objects[0].indices == baked.objects[0].indices && std::strcmp(loaded.objects[0].name, "Floor") == 0);
}

static void test_hlod() {  // HLOD: cells, merge by look, the switch distance, the file
    using namespace lsd;
    Allocator& allocator = default_allocator();
    render::MeshData ball = render::make_sphere(allocator, 16, 8);
    auto source = [&](const char* name, f32 x, const char* material) {
        editor::HlodSource s;
        s.key = editor::hlod_member_key(name, "Sphere", {x, 0, 0});
        s.world = Mat4{};
        s.world.at(0, 3) = x;
        std::snprintf(s.material, sizeof s.material, "%s", material);
        s.vertices = ball.vertices;
        s.vertex_count = ball.vertex_count;
        s.indices = ball.indices;
        s.index_count = ball.index_count;
        return s;
    };
    // Three objects share the 50 m cell at the origin (two looks), one stands alone 200 m away.
    const std::vector<editor::HlodSource> sources = {source("A", 1, ""), source("B", 3, ""), source("C", 5, "m1"), source("Far", 200, "")};
    editor::HlodData data;
    editor::build_hlod(sources, 50.0f, 0.25f, data);
    CHECK(data.cells.size() == 1 && data.cells[0].members.size() == 3 && data.cells[0].proxies.size() == 2);
    CHECK(data.cell_of.count(sources[0].key) == 1 && data.cell_of.count(sources[3].key) == 0);  // the lone one is never hidden
    u32 merged = 0;
    for (const editor::HlodProxy& p : data.cells[0].proxies) merged += static_cast<u32>(p.indices.size());
    CHECK(merged > 0 && merged < 3 * ball.index_count);  // simpler than the originals together
    CHECK(data.cells[0].lo[0] < 1.0f && data.cells[0].hi[0] > 5.0f);  // world space: the objects are at x = 1..5
    CHECK(!editor::hlod_cell_far(data.cells[0], {0, 0, 0}, 100.0f) && editor::hlod_cell_far(data.cells[0], {300, 0, 0}, 100.0f));
    CHECK(!editor::hlod_cell_far(data.cells[0], {50, 0, 0}, 100.0f));  // measured to the nearest point of the cell
    const std::string path = (std::filesystem::path(LSD_TEST_TEMP_DIR) / "Hlod_BuiltData.lsdhlod").generic_string();
    CHECK(editor::save_hlod(path.c_str(), data));
    editor::HlodData loaded;
    CHECK(editor::load_hlod(path.c_str(), loaded) && loaded.cells.size() == 1 && loaded.cells[0].proxies[0].indices == data.cells[0].proxies[0].indices &&
          loaded.cell_of.count(sources[1].key) == 1 && std::strcmp(loaded.cells[0].proxies[1].material, data.cells[0].proxies[1].material) == 0);
    CHECK(!editor::load_hlod("no/such/file.lsdhlod", loaded) && !loaded.valid());
    render::free_mesh(allocator, ball);
}

static void test_references() {  // Reference Viewer: who uses what, and Replace References
    using namespace lsd;
    CHECK(editor::find_guids("x 0123456789abcdef0123456789ABCDEF y 0123").size() == 1);
    CHECK(editor::find_guids("00112233445566778899aabbccddeeff0").empty());  // 33 digits is not a GUID
    const std::filesystem::path dir = std::filesystem::path(LSD_TEST_TEMP_DIR) / "RefContent";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    for (const char* name : {"A", "B", "C"}) std::ofstream(dir / (std::string(name) + ".lsdlevel")) << "{}";
    asset::AssetRegistry registry(default_allocator(), dir.generic_string().c_str());
    registry.scan();
    char a[33] = "", b[33] = "", c[33] = "";
    for (usize i = 0; i < registry.count(); ++i) {
        const asset::AssetRecord& r = registry.get(i);
        asset::to_string(r.guid, std::strcmp(r.name, "A") == 0 ? a : std::strcmp(r.name, "B") == 0 ? b : c);
    }
    std::ofstream(dir / "A.lsdlevel") << "{\"mesh\": \"" << b << "\", \"gone\": \"ffffffffffffffffffffffffffffffff\"}";
    editor::ReferenceIndex index;
    index.build(registry);
    CHECK(index.uses(a).size() == 2 && index.used_by(b).size() == 1 && index.used_by(b)[0] == a && index.used_by(c).empty());
    CHECK(editor::replace_references(registry, b, c) == 1);
    index.build(registry);
    CHECK(index.used_by(b).empty() && index.used_by(c).size() == 1);
    std::filesystem::remove_all(dir, error);

    // Cooking: the project names the level by path, the level uses a script by GUID, the script names a
    // sound and a UI document; the document brings its stylesheet and its .js; an unused script stays out.
    const std::filesystem::path cook = std::filesystem::path(LSD_TEST_TEMP_DIR) / "CookContent";
    std::filesystem::remove_all(cook, error);
    std::filesystem::create_directories(cook / "Levels", error);
    std::filesystem::create_directories(cook / "UI", error);
    std::ofstream(cook / "Levels" / "Main.lsdlevel") << "{}";
    std::ofstream(cook / "Door.lua") << "function on_start(self) lsd.play_sound(\"Creak\"); lsd.ui_open('Menu') end";
    std::ofstream(cook / "Unused.lua") << "-- nobody asks for me";
    std::ofstream(cook / "Creak.wav") << "RIFF";
    std::ofstream(cook / "UI" / "Menu.rml") << "<rml><head><link type=\"text/rcss\" href=\"Menu.rcss\"/></head></rml>";
    std::ofstream(cook / "UI" / "Menu.rcss") << "body {}";
    std::ofstream(cook / "UI" / "Menu.js") << "function onOpen() {}";
    asset::AssetRegistry assets(default_allocator(), cook.generic_string().c_str());
    assets.scan();
    char door[33] = "";
    for (usize i = 0; i < assets.count(); ++i) {
        if (std::strcmp(assets.get(i).name, "Door") == 0) asset::to_string(assets.get(i).guid, door);
    }
    std::ofstream(cook / "Levels" / "Main.lsdlevel") << "{\"Script\": {\"script\": \"" << door << "\"}}";
    const std::vector<std::string> cooked = editor::cook_set(assets, "{\"game_default_level\": \"Content/Levels/Main.lsdlevel\"}");
    auto has = [&](const char* name) {
        for (usize i = 0; i < assets.count(); ++i) {
            char g[33];
            asset::to_string(assets.get(i).guid, g);
            if (std::strcmp(assets.get(i).path, name) == 0) return std::find(cooked.begin(), cooked.end(), std::string(g)) != cooked.end();
        }
        return false;
    };
    CHECK(has("Levels/Main.lsdlevel") && has("Door.lua") && has("Creak.wav") && has("UI/Menu.rml") && has("UI/Menu.rcss") && has("UI/Menu.js"));
    CHECK(!has("Unused.lua") && cooked.size() == 6);

    // Size Map from the level: the same six, biggest first. Asset Audit: who uses what, compiled data, unused.
    assets.scan();  // the level's new text: sizes refreshed
    char main_level[33] = "";
    const asset::AssetRecord* creak = nullptr;
    for (usize i = 0; i < assets.count(); ++i) {
        if (std::strcmp(assets.get(i).name, "Main") == 0) asset::to_string(assets.get(i).guid, main_level);
        if (std::strcmp(assets.get(i).name, "Creak") == 0) creak = &assets.get(i);
    }
    const std::vector<editor::SizeEntry> sizes = editor::size_map(assets, {main_level});
    CHECK(sizes.size() == 6 && sizes.front().bytes >= sizes.back().bytes);
    std::filesystem::create_directories(cook / "Derived", error);
    std::ofstream(cook / "Derived" / (std::string(door) + ".lsdx")) << "0123456789";
    editor::ReferenceIndex users;
    users.build(assets);
    for (const editor::AuditRow& row : editor::audit_assets(assets, users, cooked, (cook / "Derived").generic_string().c_str())) {
        const char* name = assets.get(row.record).name;
        if (std::strcmp(name, "Door") == 0) CHECK(row.users == 1 && row.derived == 10 && !row.unused);
        if (std::strcmp(name, "Unused") == 0) CHECK(row.unused && row.users == 0);
        if (std::strcmp(name, "Main") == 0) CHECK(!row.unused);
    }

    // Fix Up Redirectors: quoted paths and names follow a move / rename; GUIDs never needed it.
    std::string text = "open_level(\"Content/Levels/Main.lsdlevel\") x = 'Levels/Main' y = \"Main\"";
    CHECK(editor::fix_moved_names(text, "Levels/Main.lsdlevel", "Maps/Main.lsdlevel", false) == 2);
    CHECK(text == "open_level(\"Content/Maps/Main.lsdlevel\") x = 'Maps/Main' y = \"Main\"");
    CHECK(creak);
    const editor::Redirect moved{creak ? creak->path : "", creak ? creak->guid : asset::Guid{}};
    char rename_error[200] = "";
    CHECK(creak && assets.rename_asset(*creak, "Squeak", rename_error, sizeof rename_error));
    CHECK(editor::fix_up_redirectors(assets, {moved}) == 1);
    std::ifstream fixed(cook / "Door.lua");
    const std::string door_text((std::istreambuf_iterator<char>(fixed)), std::istreambuf_iterator<char>());
    fixed.close();
    CHECK(door_text.find("lsd.play_sound(\"Squeak\")") != std::string::npos);
    std::filesystem::remove_all(cook, error);
}

// LSD Assistant without the network: what each provider is sent, its replies read back (thinking
// blocks kept for the next turn, refusals and errors shown), code blocks, and the writable paths.
static void test_assistant() {
    using namespace lsd::editor::assistant;
    Settings claude;
    claude.key = "k";
    std::vector<Turn> turns{{true, "Make a \"door\"\nthat opens", "", {}, {}, ""}};
    const std::string body = request_body(claude, "You are LSD Assistant", turns);
    CHECK(request_url(claude) == "https://api.anthropic.com/v1/messages");
    CHECK(body.find("\"model\":\"claude-opus-5-5\"") != std::string::npos && body.find("\"cache_control\":{\"type\":\"ephemeral\"}") != std::string::npos);
    CHECK(body.find("\"effort\":\"medium\"") != std::string::npos && body.find("\"fallbacks\":\"default\"") != std::string::npos);
    CHECK(body.find("Make a \\\"door\\\"\\nthat opens") != std::string::npos);
    const std::vector<std::string> headers = request_headers(claude);
    bool version = false, key = false, beta = false;
    for (const std::string& h : headers) {
        version = version || h == "anthropic-version: 2026-10-21";
        key = key || h == "x-api-key: k";
        beta = beta || h == "anthropic-beta: server-side-fallback-2026-07-01";
    }
    CHECK(version && key && beta);
    Settings haiku = claude;
    haiku.model = "claude-haiku-4-5";  // no effort, no fallback on older models
    const std::string old = request_body(haiku, "s", turns);
    CHECK(old.find("effort") == std::string::npos && old.find("fallbacks") == std::string::npos);
    // A picture with the message: base64 PNG, an image block before the text (Anthropic) or a data URL (OpenAI).
    const u8 bytes[5] = {'M', 'a', 'n', 0xFF, 0x00};
    CHECK(base64(bytes, 3) == "TWFu" && base64(bytes, 4) == "TWFu/w==" && base64(bytes, 5) == "TWFu/wA=" && base64(bytes, 0).empty());
    std::vector<Turn> pictured{{true, "What is wrong here?", "", {}, {}, "AAAA"}};
    const std::string with_image = request_body(claude, "s", pictured);
    CHECK(with_image.find("[{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"AAAA\"}},{\"type\":\"text\"") != std::string::npos);
    Settings openai = claude;
    openai.provider = kOpenAICompatible;
    CHECK(request_body(openai, "s", pictured).find("\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}") != std::string::npos);

    const std::string reply = R"({"id":"m","type":"message","role":"assistant","content":[{"type":"thinking","thinking":"","signature":"abc"},)"
                              R"({"type":"text","text":"Here:\n```lua Content/Script/Door.lua\nfunction on_start(self) end\n```\nDone \u00e9"}],"stop_reason":"end_turn"})";
    const Reply r = parse_reply(claude, 200, reply);
    CHECK(r.ok && r.text.find("Done \xc3\xa9") != std::string::npos && r.raw.find("\"signature\":\"abc\"") != std::string::npos && r.raw.front() == '[');
    turns.push_back({false, r.text, r.raw, {}, {}, ""});
    turns.push_back({true, "thanks", "", {}, {}, ""});
    const std::string next = request_body(claude, "s", turns);
    CHECK(next.find("\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\"") != std::string::npos);  // sent back unchanged
    const std::vector<CodeBlock> blocks = code_blocks(r.text);
    CHECK(blocks.size() == 1 && blocks[0].language == "lua" && blocks[0].path == "Content/Script/Door.lua" && blocks[0].code == "function on_start(self) end\n");
    CHECK(code_blocks("```cpp\nint x;\n```")[0].path.empty());
    CHECK(!parse_reply(claude, 200, R"({"content":[],"stop_reason":"refusal","stop_details":{"type":"refusal","explanation":"no"}})").ok);
    const Reply error = parse_reply(claude, 401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    CHECK(!error.ok && error.error == "invalid x-api-key");
    CHECK(!parse_reply(claude, 0, "").ok);

    // Streamed (server-sent events): text grows as deltas come, thinking kept with its signature.
    CHECK(body.find("\"stream\":true") != std::string::npos);
    const std::string stream =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"content\":[]}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"lo \\u00e9\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_del";  // cut mid-line: not read yet
    const Reply part = parse_stream(claude, 0, stream);
    CHECK(!part.ok && part.text == "Hello \xc3\xa9");
    const std::string whole = stream.substr(0, stream.rfind("data:")) +
                              "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\ndata: {\"type\":\"message_stop\"}\n\n";
    const Reply streamed = parse_stream(claude, 200, whole);
    CHECK(streamed.ok && streamed.text == "Hello \xc3\xa9");
    CHECK(streamed.raw == "[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig\"},{\"type\":\"text\",\"text\":\"Hello \xc3\xa9\"}]");
    const std::string refused = "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}\n\ndata: {\"type\":\"message_stop\"}\n\n";
    CHECK(!parse_stream(claude, 200, refused).ok);
    CHECK(!parse_stream(claude, 529, "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n").ok);
    CHECK(parse_stream(claude, 401, R"({"type":"error","error":{"message":"invalid x-api-key"}})").error == "invalid x-api-key");  // not a stream

    Settings local;
    local.provider = kOpenAICompatible;
    local.endpoint = "http://localhost:11434/v1/";
    local.model = "llama3.1";
    CHECK(request_url(local) == "http://localhost:11434/v1/chat/completions");
    CHECK(request_headers(local).size() == 1);  // no key: no Authorization header
    CHECK(request_body(local, "sys", {{true, "hi", "", {}, {}, ""}}).find("{\"role\":\"system\",\"content\":\"sys\"}") != std::string::npos);
    const Reply o = parse_reply(local, 200, R"({"choices":[{"message":{"role":"assistant","content":"hello"}}]})");
    CHECK(o.ok && o.text == "hello");
    const Reply os = parse_stream(local, 200, "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hel\"}}]}\n\n"
                                              "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
    CHECK(os.ok && os.text == "hello" && os.raw == "hello");

    // Tools: offered when on, calls read back from both providers' streams, results sent the way each expects.
    CHECK(request_body(claude, "s", turns, true).find("\"tools\":[{\"name\":\"console\"") != std::string::npos);
    CHECK(request_body(claude, "s", turns).find("\"tools\"") == std::string::npos);
    CHECK(request_body(local, "s", {{true, "hi", "", {}, {}, ""}}, true).find("{\"type\":\"function\",\"function\":{\"name\":\"console\"") != std::string::npos);
    const Reply use = parse_stream(claude, 200,
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Starting Play.\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"console\",\"input\":{}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"command\\\":\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\" \\\"play\\\"}\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\ndata: {\"type\":\"message_stop\"}\n\n");
    CHECK(use.ok && use.text == "Starting Play." && use.calls.size() == 1 && use.calls[0].id == "tu_1" && use.calls[0].name == "console");
    CHECK(tool_arg(use.calls[0].input, "command") == "play" && tool_arg("{\"lines\":20}", "lines") == "20" && tool_arg("{}", "x").empty());
    CHECK(use.raw.find("{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"console\",\"input\":{\"command\": \"play\"}}") != std::string::npos);
    std::vector<Turn> acted{{true, "play it", "", {}, {}, ""}, {false, use.text, use.raw, use.calls, {}, ""}, {true, "", "", {}, {{"tu_1", "Play started", false}}, ""}};
    const std::string back = request_body(claude, "s", acted, true);
    CHECK(back.find("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"tu_1\",\"content\":\"Play started\"}]}") != std::string::npos);
    const Reply ocall = parse_stream(local, 200,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"read_log\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"lines\\\":5}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n");
    CHECK(ocall.ok && ocall.calls.size() == 1 && ocall.calls[0].id == "call_9" && ocall.calls[0].name == "read_log" && tool_arg(ocall.calls[0].input, "lines") == "5");
    std::vector<Turn> oacted{{true, "log?", "", {}, {}, ""}, {false, "", "", ocall.calls, {}, ""}, {true, "", "", {}, {{"call_9", "[game] ok", false}}, ""}};
    const std::string oback = request_body(local, "s", oacted, true);
    CHECK(oback.find("\"tool_calls\":[{\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"read_log\",\"arguments\":\"{\\\"lines\\\":5}\"}}]") != std::string::npos);
    CHECK(oback.find("{\"role\":\"tool\",\"tool_call_id\":\"call_9\",\"content\":\"[game] ok\"}") != std::string::npos);
    CHECK(readable_path("Content/Levels/A.lsdlevel") && !readable_path("Config/project.json") && !readable_path("Content/../Saved/editor_settings.json"));

    CHECK(writable_path("Content/Script/Door.lua") && writable_path("Source/Door.cpp"));
    CHECK(!writable_path("../evil.lua") && !writable_path("Content/../x") && !writable_path("C:/Windows/x") && !writable_path("/etc/x") && !writable_path("Config/project.json"));
    CHECK(writable_path("Content/UI/HUD.rml") && writable_path("Source/CMakeLists.txt") && !writable_path("Content/Mesh/Gun.fbx") && !writable_path("Content/Weapon/IA_Aim.lsdasset"));
}

// Shortcuts: text round trip, defaults, only changed chords saved and loaded back.
static void test_keymap() {
    using editor::Command;
    char text[32];
    editor::chord_text(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, text, sizeof text);
    CHECK(std::strcmp(text, "Ctrl+Shift+S") == 0);
    CHECK(editor::parse_chord("Ctrl+Shift+S") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S));
    CHECK(editor::parse_chord("Alt+F11") == (ImGuiMod_Alt | ImGuiKey_F11) && editor::parse_chord("Delete") == ImGuiKey_Delete);
    CHECK(editor::parse_chord("") == 0 && editor::parse_chord("Ctrl+Nope") == 0);
    CHECK(std::strcmp(editor::command_text(Command::Save), "Ctrl+S") == 0);

    editor::EditorSettings set;
    editor::set_command_chord(Command::Save, ImGuiMod_Alt | ImGuiKey_W);
    editor::set_command_chord(Command::Delete, 0);
    editor::save_keymap(set);
    CHECK(set.shortcuts.count == 2);
    editor::load_keymap(editor::EditorSettings{});  // defaults again
    CHECK(editor::command_chord(Command::Save) == (ImGuiMod_Ctrl | ImGuiKey_S));
    editor::load_keymap(set);
    CHECK(editor::command_chord(Command::Save) == (ImGuiMod_Alt | ImGuiKey_W) && editor::command_chord(Command::Delete) == 0);
    CHECK(std::strcmp(editor::command_text(Command::Delete), "") == 0);
    editor::load_keymap(editor::EditorSettings{});
}

// Landscape paint: the weights always sum to 255, painting moves toward the layer, erasing away.
static void test_landscape_paint() {
    auto sum = [](u32 p) { return (p & 0xFFu) + ((p >> 8) & 0xFFu) + ((p >> 16) & 0xFFu) + (p >> 24); };
    auto layer = [](u32 p, int k) { return (p >> (8 * k)) & 0xFFu; };
    u32 w = 255u;  // all layer 0
    w = editor::paint_weight(w, 1, 0.5f, false);
    CHECK(sum(w) == 255 && layer(w, 1) >= 126 && layer(w, 1) <= 129 && layer(w, 0) + layer(w, 1) == 255);
    w = editor::paint_weight(w, 2, 0.5f, false);  // the others give way in proportion
    CHECK(sum(w) == 255 && layer(w, 2) >= 126 && layer(w, 0) > 50 && layer(w, 1) > 50);
    for (int i = 0; i < 40; ++i) w = editor::paint_weight(w, 3, 0.3f, false);
    CHECK(sum(w) == 255 && layer(w, 3) >= 254);
    w = editor::paint_weight(w, 3, 1.0f, true);  // all of layer 3 erased: the rest goes back to layer 0
    CHECK(sum(w) == 255 && layer(w, 3) == 0 && layer(w, 0) == 255);
    w = editor::paint_weight(w, 0, 1.0f, true);  // erasing the only layer: layer 1 takes it
    CHECK(sum(w) == 255 && layer(w, 0) == 0 && layer(w, 1) == 255);
    for (u32 seed = 1; seed < 2000; ++seed) {  // any mix, any step: still 255
        u32 p = 255u;
        for (int s = 0; s < 6; ++s) p = editor::paint_weight(p, static_cast<int>((seed + s) % 4), static_cast<f32>((seed * 7 + s) % 10) / 9.0f, (seed + s) % 3 == 0);
        CHECK(sum(p) == 255);
    }
    CHECK(editor::paint_weight(0x12345678u, 7, 1.0f, false) == 0x12345678u);
}

// Project templates: each builds a playable level (one player, the right camera), and survives a save / load.
static void test_templates() {
    HeapAllocator heap("templates");
    {
        world::World w(heap);
        editor::TemplateArt art;
        std::snprintf(art.tiles, sizeof art.tiles, "%s", "0123456789abcdef0123456789abcdef");
        for (int t = 0; t < static_cast<int>(editor::Template::Count); ++t) {
            const auto kind = static_cast<editor::Template>(t);
            editor::build_template_world(w, kind, art);
            const u32 characters = w.count(reflect::type_of<world::Character>());
            CHECK(editor::template_name(kind)[0] && editor::template_description(kind)[0]);
            const bool showcase = kind == editor::Template::CharacterCustomization || kind == editor::Template::CharacterSelection;
            CHECK(characters == (kind == editor::Template::Empty || showcase ? 0u : 1u));
            if (showcase) {  // a body on a stage, its script, and the camera on it
                CHECK(w.count(reflect::type_of<world::SkeletalMesh>()) == 1 && w.count(reflect::type_of<world::Script>()) == 1);
                CHECK(w.count(reflect::type_of<world::Camera>()) == 1);
                CHECK(w.count(reflect::type_of<world::Outfit>()) == (kind == editor::Template::CharacterCustomization ? 1u : 0u));
            }
            CHECK(kind != editor::Template::Orthographic || (w.count(reflect::type_of<world::Camera>()) == 1 &&
                                                             static_cast<const world::Camera*>(w.at(reflect::type_of<world::Camera>(), 0))->orthographic));
            CHECK(w.count(reflect::type_of<world::DirectionalLight>()) == (kind == editor::Template::TwoD ? 0u : 1u));
            if (characters) {
                const world::Entity player = w.owner(reflect::type_of<world::Character>(), 0);
                const auto* c = w.get<world::Character>(player);
                const bool side = kind == editor::Template::SideScrolling || kind == editor::Template::TwoD || kind == editor::Template::TwoPointFiveD;
                CHECK(c->side_scroller == side);
                CHECK(!side || w.count(reflect::type_of<world::Camera>()) == 1);  // the follow camera
                CHECK(kind != editor::Template::ThirdPerson || c->camera_distance > 0.0f);
            }
            if (kind == editor::Template::TwoD) {
                const auto* map = static_cast<const world::TileMap*>(w.at(reflect::type_of<world::TileMap>(), 0));
                CHECK(map && map->tiles.count == map->width * map->height && std::strcmp(map->tileset, art.tiles) == 0);
                CHECK(map && map->tiles[0] == 2 && map->tiles[map->width] == 1);  // dirt under grass at the start
                CHECK(map && map->tiles[map->width + 19] == 0);                  // a pit
                const auto* hero = w.get<world::Sprite>(w.owner(reflect::type_of<world::Character>(), 0));
                CHECK(hero && hero->walk_count == 4 && hero->jump_count == 1 && hero->columns == 4 && hero->rows == 2);
            }
            const std::string path = std::string(LSD_TEST_TEMP_DIR) + "/template_test.lsdlevel";
            CHECK(world::save_level_file(w, path.c_str()));
            world::World loaded(heap);
            CHECK(world::load_level_file(loaded, path.c_str()));
            CHECK(loaded.count(reflect::type_of<world::Character>()) == characters);
            std::filesystem::remove(path);
        }
    }
    // The 2D art: drawn cells on a see-through background.
    std::vector<u8> hero(static_cast<usize>(editor::kHeroWidth) * editor::kHeroHeight * 4);
    editor::template_hero(hero.data());
    CHECK(hero[3] == 0);  // the top-left corner is transparent
    u32 solid = 0;
    for (usize i = 3; i < hero.size(); i += 4) solid += hero[i] == 255 ? 1u : 0u;
    CHECK(solid > 1000);  // eight figures
    std::vector<u8> tiles(static_cast<usize>(editor::kTilesetSize) * editor::kTilesetSize * 4);
    editor::template_tileset(tiles.data());
    CHECK(tiles[3] == 255 && tiles[1] > tiles[0]);  // the grass tile: opaque, green on top
}

// Tile map layers: layer 0 is the map's own tiles, the others its Layers; a layer's tiles give their own runs.
static void test_tile_layers() {
    world::TileMap map;
    map.width = 4;
    map.height = 2;
    for (u32 i = 0; i < 8; ++i) map.tiles.push_back(i < 4 ? 1 : 0);  // the bottom row solid
    world::TileLayer deco;
    for (u32 i = 0; i < 8; ++i) deco.tiles.push_back(i == 5 || i == 6 ? 2 : 0);  // two tiles on the second row
    map.layers.push_back(deco);
    CHECK(editor::layer_tiles(map, 0) == &map.tiles && editor::layer_tiles(map, 1) == &map.layers[0].tiles && !editor::layer_tiles(map, 2));
    editor::TileRun runs[8];
    CHECK(editor::tile_runs(map, runs, 8) == 1 && std::fabs(runs[0].hi.x - 4.0f) < 1e-5f);
    CHECK(editor::tile_runs(map, map.layers[0].tiles, runs, 8) == 1 && std::fabs(runs[0].lo.x - 1.0f) < 1e-5f && std::fabs(runs[0].lo.z - 1.0f) < 1e-5f);
}

// Modeling: a box extruded, inset, bevelled and picked stays a closed, outward-facing mesh.
static void test_mesh_edit() {
    world::EditableMesh m;
    editor::make_box(m);
    CHECK(m.points.count == 8 && m.faces.count == 6);
    const Vec3 top = editor::face_normal(m, 0);
    CHECK(std::fabs(top.z - 1.0f) < 1e-5f);  // face 0 is the top, facing up
    for (u32 f = 0; f < m.faces.count; ++f) CHECK(dot(editor::face_normal(m, f), editor::face_centre(m, f) - Vec3{0, 0, 0.5f}) > 0.0f);  // all outward
    CHECK(editor::extrude(m, 0, 1.0f));
    CHECK(m.points.count == 12 && m.faces.count == 10 && std::fabs(editor::face_centre(m, 0).z - 2.0f) < 1e-5f);
    for (u32 f = 0; f < m.faces.count; ++f) CHECK(dot(editor::face_normal(m, f), editor::face_centre(m, f) - Vec3{0, 0, 1.0f}) > 0.0f);  // the walls face out too
    CHECK(editor::inset(m, 0, 0.5f));
    CHECK(m.faces.count == 14 && std::fabs(editor::face_normal(m, 0).z - 1.0f) < 1e-5f);
    const u32 faces = m.faces.count, points = m.points.count;
    CHECK(editor::bevel(m, 0, 0.2f, 0.3f));
    CHECK(m.faces.count == faces + 8 && m.points.count == points + 8);
    f32 t = 0.0f;
    CHECK(editor::pick_face(m, {0, 0, 10}, {0, 0, -1}, t) == 0 && std::fabs(t - (10.0f - 2.3f)) < 1e-4f);  // straight down onto the bevelled top
    CHECK(editor::pick_face(m, {5, 5, 10}, {0, 0, -1}, t) == -1);
    CHECK(editor::push_pull(m, 0, 0.2f) && std::fabs(editor::face_centre(m, 0).z - 2.5f) < 1e-4f);
    CHECK(editor::flip_face(m, 0) && editor::face_normal(m, 0).z < -0.99f);
    CHECK(editor::delete_face(m, 0) && m.faces.count == faces + 7);
    render::MeshData data;
    editor::build_edit_mesh(m, data);
    CHECK(data.vertex_count == data.index_count && data.index_count == m.faces.count * 6);  // all quads: 2 triangles each
    render::free_mesh(default_allocator(), data);
    world::EditableMesh box;  // deleting a box corner takes the three faces meeting there
    editor::make_box(box);
    CHECK(editor::delete_point(box, 7) && box.faces.count == 3 && !editor::delete_point(box, 7));
    editor::make_box(box);  // an edge belongs to two faces
    CHECK(editor::delete_edge(box, 4, 5) && box.faces.count == 4);

    // Booleans: a box against a copy moved half a metre along X.
    auto x_range = [](const world::EditableMesh& mesh, f32& lo, f32& hi) {
        lo = 1e9f;
        hi = -1e9f;
        for (u32 f = 0; f < mesh.faces.count; ++f) {
            for (u32 i = 0; i < editor::face_size(mesh.faces[f]); ++i) {
                const f32 x = editor::face_point(mesh, mesh.faces[f], i).x;
                lo = std::fmin(lo, x);
                hi = std::fmax(hi, x);
            }
        }
    };
    const Mat4 shifted = translation({0.5f, 0.0f, 0.0f});
    world::EditableMesh tool;
    editor::make_box(tool);
    f32 lo = 0.0f, hi = 0.0f;
    world::EditableMesh u;
    editor::make_box(u);
    CHECK(editor::boolean_op(u, tool, shifted, editor::BooleanOp::Union));
    x_range(u, lo, hi);
    CHECK(std::fabs(lo + 0.5f) < 1e-4f && std::fabs(hi - 1.0f) < 1e-4f);
    world::EditableMesh sub;
    editor::make_box(sub);
    CHECK(editor::boolean_op(sub, tool, shifted, editor::BooleanOp::Subtract));
    x_range(sub, lo, hi);
    CHECK(std::fabs(lo + 0.5f) < 1e-4f && std::fabs(hi) < 1e-4f);  // what is left: the box's -X half
    world::EditableMesh both;
    editor::make_box(both);
    CHECK(editor::boolean_op(both, tool, shifted, editor::BooleanOp::Intersect));
    x_range(both, lo, hi);
    CHECK(std::fabs(lo) < 1e-4f && std::fabs(hi - 0.5f) < 1e-4f);
    for (u32 f = 0; f < sub.faces.count; ++f) CHECK(dot(editor::face_normal(sub, f), editor::face_centre(sub, f) - Vec3{-0.25f, 0, 0.5f}) > 0.0f);  // still facing out
    world::EditableMesh apart;  // nothing in common: an empty intersection leaves the mesh as it was
    editor::make_box(apart);
    CHECK(!editor::boolean_op(apart, tool, translation({5.0f, 0.0f, 0.0f}), editor::BooleanOp::Intersect) && apart.faces.count == 6);
    const std::string obj = editor::to_obj(m, "Box");
    CHECK(obj.find("\nv ") != std::string::npos && obj.find("\nf ") != std::string::npos);
}

// Landscape: a heightmap image in and out again, and holes that remove the squares around them.
static void test_landscape_core() {
    constexpr u32 kGrid = world::Landscape::kResolution;
    std::vector<u8> ramp(16 * 8 * 4);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            u8* p = &ramp[(y * 16 + x) * 4];
            p[0] = p[1] = p[2] = static_cast<u8>(x * 17);  // black on the left (-X) to white on the right
            p[3] = 255;
        }
    }
    world::Landscape l;
    editor::heights_from_image(l, ramp.data(), 16, 8, 10.0f);
    CHECK(l.heights.count == kGrid * kGrid);
    CHECK(std::fabs(l.heights[0]) < 1e-4f && std::fabs(l.heights[kGrid - 1] - 10.0f) < 1e-3f);
    CHECK(l.heights[kGrid / 2] > 4.0f && l.heights[kGrid / 2] < 6.0f);
    std::vector<u8> out(static_cast<usize>(kGrid) * kGrid * 4);
    editor::image_from_heights(l, 10.0f, out.data());
    CHECK(out[0] == 0 && out[(kGrid - 1) * 4] == 255 && out[3] == 255);
    // 16 bits (Unreal's .r16): a height comes back within 1 / 65535 of the range, through a file too.
    l.heights[7 * kGrid + 9] = 12.3456f;
    std::vector<u16> samples(static_cast<usize>(kGrid) * kGrid);
    editor::samples_from_heights(l, 50.0f, samples.data());
    const std::string r16 = LSD_TEST_TEMP_DIR "/landscape_test.r16";
    CHECK(editor::write_r16(r16.c_str(), samples.data(), kGrid));
    std::vector<u16> read;
    u32 side = 0;
    CHECK(editor::read_r16(r16.c_str(), read, side) && side == kGrid && read == samples);
    world::Landscape back;
    editor::heights_from_samples(back, read.data(), side, side, 50.0f);
    CHECK(std::fabs(back.heights[7 * kGrid + 9] - 12.3456f) < 50.0f / 65535.0f + 1e-4f);
    CHECK(std::fabs(back.heights[kGrid - 1] - 10.0f) < 1e-3f);  // the ramp's top corner
    std::remove(r16.c_str());
    CHECK(l.holes.count == 0 && editor::quad_open(l, 3, 3));
    editor::set_hole(l, 4, 4, true);
    CHECK(l.holes.count == kGrid * kGrid && editor::is_hole(l, 4, 4));
    CHECK(!editor::quad_open(l, 3, 3) && !editor::quad_open(l, 4, 4) && editor::quad_open(l, 5, 5));  // the four squares around it
    editor::set_hole(l, 4, 4, false);
    CHECK(editor::quad_open(l, 3, 3));
}

// Gather Text: keys from scripts and UI texts, merged into the string table without duplicates.
static void test_gather_text() {
    std::vector<editor::GatheredText> found;
    editor::gather_script_text("local t = lsd.text(\"MENU_PLAY\") .. lsd.text( 'SCORE', 3) .. lsd.text(name)", found);
    CHECK(found.size() == 2 && found[0].key == "MENU_PLAY" && found[1].key == "SCORE");
    editor::gather_rml_text("<rml><head><title>Menu</title><style>body { color: red; }</style></head><body>"
                            "<button id=\"go\">Start  Game</button><div>{{game.score}}</div><div> 42 </div><p>Quit &amp; Save</p></body></rml>",
                            found);
    CHECK(found.size() == 4 && found[2].key == "Start Game" && found[2].text == "Start Game" && found[3].key == "Quit & Save");
    std::string csv;
    CHECK(editor::merge_string_table("Key,en,fr\nMENU_PLAY,Play,Jouer\n", found, csv) == 3);
    CHECK(csv.find("MENU_PLAY,Play,Jouer") != std::string::npos && csv.find("Start Game,Start Game,") != std::string::npos);
    std::string again;
    CHECK(editor::merge_string_table(csv, found, again) == 0);
    std::string fresh;
    CHECK(editor::merge_string_table("", found, fresh) == 4 && fresh.rfind("Key,en\n", 0) == 0);
}

// Run Behavior: another tree's branch is copied in where the node is, glowing as that node.
static void test_run_behavior() {
    HeapAllocator heap("bt");
    script::Graph sub(heap);
    const u32 sub_root = sub.add("BtRoot", 0, 0)->id;
    script::GraphNode* wait = sub.add("BtWait", 0, 0);
    std::snprintf(wait->values[1], sizeof wait->values[1], "%s", "3");
    CHECK(sub.connect(sub_root, 0, wait->id, 0));
    static std::string sub_path;
    sub_path = std::string(LSD_TEST_TEMP_DIR) + "/BT_Sub.lsdbt";
    CHECK(script::save_graph_file(sub, sub_path.c_str()));
    script::Graph main_tree(heap);
    const u32 root = main_tree.add("BtRoot", 0, 0)->id;
    const u32 seq = main_tree.add("BtSequence", 0, 0)->id;
    script::GraphNode* run = main_tree.add("BtRunBehavior", 0, 0);
    CHECK(run != nullptr);
    if (!run) return;
    std::snprintf(run->values[1], sizeof run->values[1], "%s", "BT_Sub");
    const u32 run_id = run->id;
    const u32 set = main_tree.add("BtSetKey", 0, 0)->id;
    CHECK(main_tree.connect(root, 0, seq, 0) && main_tree.connect(seq, 0, run_id, 0) && main_tree.connect(seq, 1, set, 0));
    ai::BehaviorTree tree;
    char error[256] = "";
    auto resolve = [](void*, const char* name, char* out, usize capacity) {
        if (std::strcmp(name, "BT_Sub") != 0) return false;
        std::snprintf(out, capacity, "%s", sub_path.c_str());
        return true;
    };
    CHECK(editor::build_behavior_tree(main_tree, tree, error, sizeof error, resolve, nullptr));
    bool copied = false;
    for (u32 i = 0; i < tree.count; ++i) copied |= tree.nodes[i].type == ai::BtType::Wait && tree.nodes[i].value == 3.0f && tree.nodes[i].id == run_id;
    CHECK(copied && tree.count == 5);  // root, sequence, run behavior (a one-child sequence), wait, set key
    CHECK(!editor::build_behavior_tree(main_tree, tree, error, sizeof error) && error[0]);  // nothing to find the tree with
    std::filesystem::remove(sub_path);
}

// A State Tree asset becomes the runner's tree: tasks by kind, transitions by state name (an unknown name fails).
static void test_state_tree_asset() {
    auto data = std::make_unique<world::StateTreeData>();
    data->states.count = 2;
    std::snprintf(data->states[0].name, sizeof data->states[0].name, "Idle");
    std::snprintf(data->states[1].name, sizeof data->states[1].name, "Sit");
    data->states[0].tasks.count = 1;
    data->states[0].tasks[0].kind = 1;  // Wait
    data->states[0].tasks[0].value = 2.0f;
    data->states[0].transitions.count = 1;
    std::snprintf(data->states[0].transitions[0].to_state, sizeof data->states[0].transitions[0].to_state, "Sit");
    data->states[1].tasks.count = 1;
    data->states[1].tasks[0].kind = 6;  // Use Smart Object
    std::snprintf(data->states[1].tasks[0].text, sizeof data->states[1].tasks[0].text, "Bench");
    auto tree = std::make_unique<ai::StateTree>();
    char error[128] = "";
    CHECK(editor::build_state_tree(*data, *tree, error, sizeof error));
    CHECK(tree->count == 2 && tree->states[0].tasks[0].type == ai::BtType::Wait && tree->states[0].transitions[0].target == 1);
    CHECK(tree->states[0].transitions[0].trigger == ai::StTrigger::Completed);
    CHECK(tree->states[1].tasks[0].type == ai::BtType::UseSmartObject && std::strcmp(tree->states[1].tasks[0].text, "Bench") == 0);
    std::snprintf(data->states[0].transitions[0].to_state, sizeof data->states[0].transitions[0].to_state, "Nowhere");
    CHECK(!editor::build_state_tree(*data, *tree, error, sizeof error) && std::strstr(error, "Nowhere"));

    world::EnvQueryData q;
    q.tests.count = 1;
    q.tests[0].purpose = 1;  // Filter only
    const ai::EnvQuery eq = editor::to_env_query(q);
    CHECK(eq.test_count == 1 && eq.tests[0].filter && !eq.tests[0].score && eq.points_per_ring == 12);
}

// Device Profiles: GPU name (any case), video memory range and platform; applying sets the quality and groups.
static void test_device_profiles() {
    editor::DeviceProfile deck;
    std::snprintf(deck.gpu_contains, sizeof deck.gpu_contains, "%s", "van gogh");
    deck.platform = 2;
    deck.quality = 1;
    deck.shadow_quality = 1;
    CHECK(editor::profile_matches(deck, "AMD Custom GPU 0405 (Van Gogh)", 1024ull << 20, 2));
    CHECK(!editor::profile_matches(deck, "AMD Custom GPU 0405 (Van Gogh)", 1024ull << 20, 1));  // not on Windows
    CHECK(!editor::profile_matches(deck, "NVIDIA GeForce RTX 4090", 24ull << 30, 2));
    editor::DeviceProfile small;
    small.max_video_memory_mb = 4096;
    small.quality = 0;
    CHECK(editor::profile_matches(small, "Any GPU", 3ull << 30, 1) && !editor::profile_matches(small, "Any GPU", 8ull << 30, 1));
    editor::GameUserSettings s;
    editor::apply_profile(deck, s);
    CHECK(s.quality == 1 && s.shadow_quality == 1 && s.post_quality == 0);
}

// Engine Scalability: groups follow the overall quality unless set, and Low trims the post pass.
static void test_scalability() {
    editor::GameUserSettings s;
    s.quality = 1;
    s.shadow_quality = 4;  // Epic on its own
    const editor::Scalability q = editor::resolve_scalability(s);
    CHECK(q.shadows == 3 && q.post == 1 && q.view_distance == 1 && q.effects == 1);
    CHECK(editor::view_distance_scale(0) < editor::view_distance_scale(3) && editor::effects_scale(3) == 1.0f);
    rhi::PostDraw post;
    post.taa = true;
    post.fxaa = false;
    post.ao_intensity = post.ssr_intensity = 1.0f;
    post.dof_focal_distance = 5.0f;
    rhi::PostDraw high = post;
    editor::limit_post(2, high);
    CHECK(high.taa && high.ssr_intensity == 1.0f);  // High: as the volume sets it
    editor::limit_post(1, post);
    CHECK(!post.taa && post.fxaa && post.ssr_intensity == 0.0f && post.dof_focal_distance == 0.0f && post.ao_intensity == 1.0f);
    editor::limit_post(0, post);
    CHECK(post.ao_intensity == 0.0f);
}

// 2D: a Tile Map's collision from its tile shapes, and Flipbook timing.
static void test_paper2d() {
    world::TileMap map;
    map.width = 6;
    map.height = 1;
    map.tile_size = 1.0f;
    for (u32 i = 0; i < 6; ++i) map.tiles.push_back(0);
    map.tiles[0] = map.tiles[1] = 1;  // two Full tiles: one box
    map.tiles[2] = 2;                 // tile 2: a slope
    map.tiles[3] = 3;                 // tile 3: decoration
    map.tiles[4] = map.tiles[5] = 4;  // tile 4: top halves, one box
    for (u32 i = 0; i < 4; ++i) map.tile_shapes.push_back({});
    map.tile_shapes[1].shape = editor::kTileSlopeUp;
    map.tile_shapes[2].shape = editor::kTileNone;
    map.tile_shapes[3].shape = editor::kTileTopHalf;
    editor::TileRun runs[8];
    const u32 n = editor::tile_runs(map, runs, 8);
    CHECK(n == 3);
    CHECK(n == 3 && runs[0].kind == 0 && runs[0].lo.x == 0.0f && runs[0].hi.x == 2.0f && runs[0].hi.z == 1.0f);
    CHECK(n == 3 && runs[1].kind == 1 && runs[1].lo.x == 2.0f && runs[1].hi.x == 3.0f);
    CHECK(n == 3 && runs[2].kind == 0 && runs[2].lo.x == 4.0f && runs[2].hi.x == 6.0f && runs[2].lo.z == 0.5f);
    CHECK(editor::tile_shape(map, 0) == editor::kTileNone && editor::tile_shape(map, 9) == editor::kTileFull);

    world::Flipbook fb;
    CHECK(editor::flipbook_frame(fb, 1.0f) == -1);
    for (u32 i = 0; i < 3; ++i) fb.frames.push_back({});
    fb.frames[0].duration = 0.1f;
    fb.frames[1].duration = 0.3f;  // a held frame
    fb.frames[2].duration = 0.1f;
    CHECK(editor::flipbook_frame(fb, 0.05f) == 0);
    CHECK(editor::flipbook_frame(fb, 0.2f) == 1 && editor::flipbook_frame(fb, 0.39f) == 1);
    CHECK(editor::flipbook_frame(fb, 0.45f) == 2);
    CHECK(editor::flipbook_frame(fb, 0.55f) == 0);  // looped
    fb.loop = false;
    CHECK(editor::flipbook_frame(fb, 5.0f) == 2);   // held on the last frame
}

void test_post_blend() {
    world::World w(default_allocator());
    world::PostProcessVolume out;
    CHECK(!editor::blend_post_volumes(w, {0, 0, 0}, false, out) && out.saturation == 1.0f);  // none: the defaults
    const world::Entity global = w.create("Global");
    w.add<world::PostProcessVolume>(global)->saturation = 0.0f;  // unbound, weight 1: black and white everywhere
    const world::Entity box = w.create("Box");
    auto* local = w.add<world::PostProcessVolume>(box);
    local->unbound = false;
    local->priority = 1;
    local->saturation = 2.0f;
    local->blend_radius = 2.0f;
    local->filmic_tonemapper = false;
    w.get<world::Transform>(box)->scale = {2, 2, 2};  // x, y -1..1, z 0..2
    CHECK(editor::blend_post_volumes(w, {0, 0, 1}, false, out) && std::fabs(out.saturation - 2.0f) < 1e-5f && !out.filmic_tonemapper);  // inside: on top
    CHECK(editor::blend_post_volumes(w, {2, 0, 1}, false, out) && std::fabs(out.saturation - 1.0f) < 1e-5f && !out.filmic_tonemapper);  // 1 m out of 2: half
    CHECK(editor::blend_post_volumes(w, {4, 0, 1}, false, out) && std::fabs(out.saturation) < 1e-5f && out.filmic_tonemapper);  // beyond the radius: the global one
    local->blend_weight = 0.25f;
    CHECK(editor::blend_post_volumes(w, {0, 0, 1}, false, out) && std::fabs(out.saturation - 0.5f) < 1e-5f && out.filmic_tonemapper);  // a quarter in: switches stay
}

void test_bake_texture_albedo() {  // Build Lighting: a triangle's albedo takes its material's base colour texture
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(LSD_TEST_TEMP_DIR) / "BakeAlbedo";
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    u8 red[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) red[i * 4] = 255, red[i * 4 + 1] = 0, red[i * 4 + 2] = 0, red[i * 4 + 3] = 255;
    CHECK(asset::write_png((dir / "T_Red.png").string().c_str(), 4, 4, red));
    asset::AssetRegistry registry(default_allocator(), dir.generic_string().c_str());
    registry.scan();
    char texture[33] = "";
    for (usize i = 0; i < registry.count(); ++i) {
        if (std::strcmp(registry.get(i).name, "T_Red") == 0) asset::to_string(registry.get(i).guid, texture);
    }
    std::ofstream(dir / "M_Red.lsdasset") << "{\"type\": \"Material\", \"data\": {\"base_color_texture\": \"" << texture << "\"}}";
    registry.scan();
    char material[33] = "";
    for (usize i = 0; i < registry.count(); ++i) {
        if (std::strcmp(registry.get(i).name, "M_Red") == 0) asset::to_string(registry.get(i).guid, material);
    }
    world::World w(default_allocator());
    auto* mesh = w.add<world::StaticMesh>(w.create("Box"));
    std::snprintf(mesh->mesh, sizeof mesh->mesh, "Cube");
    std::snprintf(mesh->material, sizeof mesh->material, "%s", material);
    mesh->color = LinearColor::hex(0xFFFFFF);  // no tint: the texture's own colour
    editor::BakeScene scene;
    CHECK(texture[0] && material[0] && editor::gather_bake_scene(w, registry, dir.generic_string().c_str(), default_allocator(), scene) && !scene.albedo.empty());
    if (!scene.albedo.empty()) CHECK(scene.albedo[0].x > 0.9f && scene.albedo[0].y < 0.05f && scene.albedo[0].z < 0.05f);
    fs::remove_all(dir, error);
}

void test_push_helpers() {  // Remote Console > Push Changes: what may be written, and the bytes back
    std::string path, bytes;
    CHECK(editor::assistant::safe_content_path("Scripts\\Door.lua", path) && path == "Scripts/Door.lua");
    CHECK(!editor::assistant::safe_content_path("../Config/x", path) && !editor::assistant::safe_content_path("/etc/passwd", path) && !editor::assistant::safe_content_path("C:/x", path) &&
          !editor::assistant::safe_content_path("a//b", path) && !editor::assistant::safe_content_path("a/./b", path) && !editor::assistant::safe_content_path("", path));
    const u8 data[5] = {0, 255, 'h', 'i', '!'};
    CHECK(editor::assistant::base64_decode(editor::assistant::base64(data, 5).c_str(), bytes) && bytes.size() == 5 && std::memcmp(bytes.data(), data, 5) == 0);
    CHECK(!editor::assistant::base64_decode("ab$c", bytes));
}

void test_landscape_tiles() {  // Add Tile: the new tile's shared edge is its neighbour's, continued across
    constexpr u32 n = world::Landscape::kResolution;
    static world::Landscape from, to;
    from.size = 32.0f;
    from.heights.count = n * n;
    for (u32 j = 0; j < n; ++j)
        for (u32 i = 0; i < n; ++i) from.heights[j * n + i] = static_cast<f32>(i) * 0.1f + static_cast<f32>(j);
    editor::extend_tile(from, 1, 0, to);  // to +X: its first column is from's last
    CHECK(to.size == 32.0f && to.heights.count == n * n);
    CHECK(to.heights[5 * n + 0] == from.heights[5 * n + n - 1] && to.heights[5 * n + 30] == from.heights[5 * n + n - 1]);
    editor::extend_tile(from, 0, -1, to);  // to -Y: its last row is from's first
    CHECK(to.heights[(n - 1) * n + 7] == from.heights[7] && to.heights[3 * n + 7] == from.heights[7]);
}

void test_shadow_cells() {  // Shadow Resolution: aligned squares in the 8 x 8 cell atlas
    editor::ShadowCells cells;
    f32 x = -1, y = -1;
    CHECK(cells.take(4, x, y) && x == 0.0f && y == 0.0f);   // High: a quarter of the atlas
    CHECK(cells.take(4, x, y) && x == 0.5f && y == 0.0f);
    CHECK(cells.take(2, x, y) && x == 0.0f && y == 0.5f);   // Medium: the old quarter-size tiles
    CHECK(cells.take(1, x, y) && x == 0.25f && y == 0.5f);  // Low
    CHECK(cells.take(4, x, y) && x == 0.5f && y == 0.5f);
    CHECK(!cells.take(4, x, y));                            // full for High
    cells.give(0.5f, 0.5f, 4);
    CHECK(cells.take(4, x, y) && x == 0.5f && y == 0.5f);   // given back, taken again
    // Through world_lights: a High spot gets a half-atlas tile, a Low one an eighth, and a High point light (six views
    // of half the atlas cannot fit) falls back to Medium quarters.
    editor::set_shadow_quality(2);
    world::World w(default_allocator());
    auto spot = [&](const char* name, u32 resolution, Vec3 at) {
        const world::Entity e = w.create(name);
        w.get<world::Transform>(e)->position = at;
        w.add<world::SpotLight>(e)->shadow_resolution = resolution;
    };
    spot("High", 2, {0, 0, 2});
    spot("Low", 0, {1, 0, 2});
    const world::Entity point = w.create("Point");
    w.get<world::Transform>(point)->position = {2, 0, 2};
    w.add<world::PointLight>(point)->shadow_resolution = 2;
    editor::LightView view;
    view.eye = {0, -5, 2};
    const rhi::LightSetup s = editor::world_lights(w, view, false);
    f32 sizes[3] = {};
    for (u32 i = 0; i < s.count; ++i) {
        if (s.lights[i].shadow_tile < 0.0f) continue;
        const f32 size = s.local_tiles[static_cast<u32>(s.lights[i].shadow_tile)][2];
        sizes[s.lights[i].type == static_cast<f32>(rhi::LightType::Point) ? 2 : s.lights[i].position[0] < 0.5f ? 0 : 1] = size;
    }
    CHECK(sizes[0] == 0.5f && sizes[1] == 0.125f && sizes[2] == 0.25f && s.local_tiles[0][3] == 8.0f);  // 1 + 1 + 6 tiles
}

void test_particle_light() {
    rhi::LightSetup s;
    s.exposure = 2.0f;
    s.count = 0;
    for (int c = 0; c < 3; ++c) s.sky[c] = 0.5f, s.ground[c] = 0.1f;
    const Vec3 up = editor::particle_light(s, {0, 0, 0}, {0, 0, 1});
    CHECK(std::fabs(up.x - 1.0f) < 1e-5f);  // the sky's ambient times exposure
    s.count = 1;  // a point light 2 m away along +X, range 10, colour 4 pi
    s.lights[0] = rhi::GpuLight{};
    s.lights[0].type = 1.0f;
    s.lights[0].position[0] = 2.0f;
    s.lights[0].range = 10.0f;
    for (int c = 0; c < 3; ++c) s.lights[0].color[c] = 4.0f * kPi;
    const Vec3 facing = editor::particle_light(s, {0, 0, 0}, {1, 0, 0});
    const Vec3 away = editor::particle_light(s, {0, 0, 0}, {-1, 0, 0});
    const f32 r = 4.0f / 100.0f, window = (1.0f - r * r) * (1.0f - r * r);
    const f32 ambient = 2.0f * (0.1f + 0.4f * 0.5f);
    CHECK(std::fabs(facing.x - (ambient + 2.0f * 4.0f * window / 4.0f)) < 1e-4f);  // wrap 1: 4 pi / pi / 4 m2 times the window, times exposure
    CHECK(std::fabs(away.x - ambient) < 1e-4f && facing.x > away.x);  // from behind: wrap 0
    s.lights[0].type = 0.0f;  // a sun shining down, the shadowed one: behind a wall only the ambient is left
    s.lights[0].direction[0] = 0.0f;
    s.lights[0].direction[2] = -1.0f;
    s.shadow_info[1] = 0.0f;
    const Vec3 up_n{0, 0, 1};
    CHECK(editor::particle_light(s, {0, 0, 0}, up_n, 1.0f).x > editor::particle_light(s, {0, 0, 0}, up_n, 0.0f).x);
    CHECK(std::fabs(editor::particle_light(s, {0, 0, 0}, up_n, 0.0f).x - 2.0f * 0.5f) < 1e-4f);
}

// PCG Graphs: a 10 x 10 m box over flat ground (x < 0) and a 45 degree slope (x > 0); the Slope Filter keeps the flat
// half, the Density Filter about half of that, an exclusion box empties a quarter of the box, the seed repeats a layout.
static void test_pcg_graph() {
    using namespace lsd;
    script::Graph graph(default_allocator());
    const u32 sample = graph.add("PcgSurface", 0, 0)->id;
    const u32 slope = graph.add("PcgSlope", 0, 0)->id;
    const u32 thin = graph.add("PcgDensity", 0, 0)->id;
    const u32 vary = graph.add("PcgTransform", 0, 0)->id;
    const u32 spawn = graph.add("PcgSpawn", 0, 0)->id;
    const u32 rocks = graph.add("PcgSpawn", 0, 0)->id;
    std::snprintf(graph.find(sample)->values[0], 64, "400");  // per 100 square metres: 400 points in the box
    CHECK(graph.connect(sample, 0, slope, 0) && graph.connect(slope, 0, vary, 0) && graph.connect(vary, 0, spawn, 0));
    CHECK(graph.connect(slope, 0, thin, 0) && graph.connect(thin, 0, rocks, 0));
    CHECK(!graph.connect(sample, 0, spawn, 1));  // points never go into a text input
    std::snprintf(graph.find(rocks)->values[1], 64, "Cube");
    editor::PcgInputs in;
    in.box = scaling(Vec3{10, 10, 10}) * translation(Vec3{0, 0, -0.5f});  // z from -5 to 5
    in.surface = [](Vec3 top, Vec3 down, f32 reach, Vec3& hit) {  // ground: z = 0 for x < 0, z = x beyond
        const f32 ground = top.x < 0.0f ? 0.0f : top.x;
        const f32 t = (top.z - ground) / -down.z;
        if (t < 0.0f || t > reach) return false;
        hit = top + down * t;
        return true;
    };
    std::vector<editor::PcgSpawn> out;
    char error[256];
    CHECK(editor::run_pcg_graph(graph, in, out, error, sizeof error) && out.size() == 2);
    if (out.size() == 2) {
        const usize flat = out[0].points.size(), thinned = out[1].points.size();
        CHECK(flat > 140 && flat < 260);  // about half of 400 (the slope's half is filtered out)
        bool on_flat = true, varied = false;
        for (const editor::PcgPoint& p : out[0].points) {
            on_flat = on_flat && p.position.x < 0.3f && std::fabs(p.position.z) < 1e-3f;
            varied = varied || std::fabs(p.scale - 1.0f) > 1e-3f;
        }
        CHECK(on_flat && varied);
        CHECK(thinned > flat / 4 && thinned < flat * 3 / 4);  // Keep 0.5
        CHECK(std::strcmp(out[1].mesh, "Cube") == 0 && std::strcmp(out[0].mesh, "Sphere") == 0);
        std::vector<editor::PcgSpawn> again;
        editor::run_pcg_graph(graph, in, again, error, sizeof error);
        CHECK(again.size() == 2 && again[0].points.size() == flat && std::fabs(again[0].points[0].position.y - out[0].points[0].position.y) < 1e-6f);
        in.excluded.push_back(affine_inverse(translation(Vec3{-2.5f, -2.5f, -5.0f}) * scaling(Vec3{5, 5, 10})));  // x, y in -5..0
        editor::run_pcg_graph(graph, in, again, error, sizeof error);
        CHECK(again.size() == 2 && again[0].points.size() < flat * 3 / 4 && again[0].points.size() > flat / 4);
    }
    const u32 merge = graph.add("PcgMerge", 0, 0)->id, back = graph.add("PcgMerge", 0, 0)->id;  // a loop is an error, not a hang
    CHECK(graph.connect(merge, 0, back, 0) && graph.connect(back, 0, merge, 0) && graph.connect(merge, 0, spawn, 0));
    CHECK(!editor::run_pcg_graph(graph, in, out, error, sizeof error) && std::strstr(error, "loop"));
}

// Cloth meshes: a 1 m quad drawn as two triangles with a seam (6 vertices, the diagonal's corners twice),
// subdivided twice: 32 triangles over a 5 x 5 grid of particles; Top pins its upper row.
static void test_cloth_mesh() {
    editor::ClothMesh m;
    const f32 corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corners) m.vertices.push_back(rhi::MeshVertex{{c[0], 0.0f, c[1]}, {0, -1, 0}, {c[0], c[1]}, {0, 0}});
    m.indices = {0, 1, 2, 3, 4, 5};
    editor::subdivide_cloth(m.vertices, m.indices, 2);
    CHECK(m.indices.size() == 32 * 3);
    editor::weld_cloth(m, 1e-4f);
    CHECK(m.particles.size() == 25 && m.particle_of.size() == m.vertices.size());
    CHECK(editor::pin_cloth(m, 0, 0.0f) == 5);   // the z = 1 row
    CHECK(editor::pin_cloth(m, 3, 0.3f) == 10);  // -X: x <= 0.3, two columns
    CHECK(editor::pin_cloth(m, 6, 0.5f) == 0);   // None
    std::vector<Vec3> moved = m.particles;
    for (Vec3& p : moved) p.y += p.z * 0.5f;  // tilted back: normals turn with it, the same on both sides of the seam
    editor::update_cloth_vertices(m, moved.data());
    bool same = true;
    for (usize v = 0; v < m.vertices.size(); ++v) same = same && std::fabs(m.vertices[v].normal[1] - m.vertices[0].normal[1]) < 1e-4f;
    CHECK(same && std::fabs(m.vertices[0].normal[1] + 0.894f) < 0.01f && std::fabs(m.vertices[0].normal[2] - 0.447f) < 0.01f);
    const rhi::MeshVertex& mid = m.vertices[m.indices[2]];
    CHECK(std::fabs(mid.uv[0] + mid.uv[1] - mid.position[0] - mid.position[2]) < 1e-5f);  // UVs follow the grid
}

// Editor utilities: a script spawns, moves and selects in a test world; the run is one undo step; its panel's
// widgets set the script's variables and its button calls a function; lsd.editor raises outside the editor.
static lsd::world::Entity named(const lsd::world::World& w, const char* name) {
    for (lsd::u32 slot = 0; slot < w.slot_count(); ++slot) {
        const lsd::world::Entity e = w.entity_at_slot(slot);
        if (e.valid() && std::strcmp(w.name(e), name) == 0) return e;
    }
    return {};
}

static void test_editor_utility() {
    using namespace lsd;
    world::World w(default_allocator());
    const world::Entity crate = w.create("Crate");
    editor::Transactions history(default_allocator());
    history.reset(w, "Open");
    struct Ctx {
        world::World* world;
        world::Entity selected[8];
        u32 count;
    } ctx{&w, {}, 0};
    ctx.selected[ctx.count++] = crate;
    editor::UtilityHooks hooks;
    hooks.user = &ctx;
    hooks.selection = [](void* user, world::Entity* out, u32 capacity) {
        auto& c = *static_cast<Ctx*>(user);
        u32 n = 0;
        for (; n < c.count && n < capacity; ++n) out[n] = c.selected[n];
        return n;
    };
    hooks.select = [](void* user, const world::Entity* list, u32 count) {
        auto& c = *static_cast<Ctx*>(user);
        c.count = count < 8 ? count : 8;
        for (u32 i = 0; i < c.count; ++i) c.selected[i] = list[i];
    };
    hooks.spawn = [](void* user, const char* name, Vec3 at) {
        world::World& world = *static_cast<Ctx*>(user)->world;
        char unique[64];
        world.unique_name(name, unique, sizeof unique);
        const world::Entity e = world.create(unique);
        world.get<world::Transform>(e)->position = at;
        return e;
    };
    editor::EditorUtilities utilities;
    utilities.start(w, hooks);
    const char* code = R"(
function run(self)
  for _, e in ipairs(lsd.editor.selection()) do lsd.set_position(e, 0, 0, 5) end
  local box = lsd.spawn("Box", 1, 2, 3)
  lsd.editor.select({box})
  lsd.editor.panel("Tools ]]", {{"label", "Hi"}, {"number", "Height", "height", 2}, {"checkbox", "Tall", "tall", true},
                                {"text", "Name", "label"}, {"button", "Raise", "raise"}})
end
function raise()
  local e = lsd.find(label)
  if tall then lsd.set_position(e, 0, 0, height) end
end
)";
    CHECK(utilities.run_code("Content/EU_Test.lua", code));  // the level changed
    history.commit(w, "Run EU_Test");
    CHECK(w.get<world::Transform>(crate)->position.z == 5.0f);
    CHECK(ctx.count == 1 && std::strcmp(w.name(ctx.selected[0]), "Box") == 0);
    CHECK(utilities.panels().size() == 1);
    if (utilities.panels().size() == 1) {
        editor::UtilityPanel& panel = utilities.panels()[0];
        CHECK(std::strcmp(panel.title, "Tools ]]") == 0 && panel.widgets.size() == 5);
        CHECK(panel.widgets[1].kind == editor::UtilityWidget::Kind::Number && panel.widgets[1].number == 2.0);
        CHECK(panel.widgets[2].kind == editor::UtilityWidget::Kind::Checkbox && panel.widgets[2].on);
        CHECK(panel.widgets[4].kind == editor::UtilityWidget::Kind::Button);
        panel.widgets[1].number = 7.5;
        utilities.push_value(0, 1);
        std::snprintf(panel.widgets[3].text, sizeof panel.widgets[3].text, "Crate");
        utilities.push_value(0, 3);
        CHECK(utilities.press(0, 4));
        CHECK(w.get<world::Transform>(crate)->position.z == 7.5f);
        history.commit(w, "Raise");
        CHECK(!utilities.press(0, 4));  // already there: no undo step
    }
    CHECK(history.undo(w));
    CHECK(w.get<world::Transform>(named(w, "Crate"))->position.z == 5.0f);
    CHECK(history.undo(w));
    CHECK(w.get<world::Transform>(named(w, "Crate"))->position.z == 0.0f && !named(w, "Box").valid());  // the spawn is undone too
    CHECK(!utilities.run_code("Content/EU_Bad.lua", "function run( broken"));  // a syntax error is logged, nothing changes
    // Play's scripts have no lsd.editor host: the call raises.
    script::ScriptSystem play;
    play.start(w, {}, nullptr, nullptr);
    CHECK(!play.run("lsd._editor('selection')"));
    play.stop();
    CHECK(editor::lua_quote("a]") == "[=[\na]]=]" && editor::lua_quote("x") == "[[\nx]]");
}

// Multi-user editing: one editor's edits as ops, applied on another's copy of the level.
static void test_multi_user_ops() {
    HeapAllocator heap("multi_user");
    world::World a(heap), b(heap);
    const world::Entity floor = a.create("Floor");
    const world::Entity crate = a.create("Crate");
    const world::Entity lamp = a.create("Lamp");
    Array<char> text(heap), other(heap);
    world::save_level(a, text);
    CHECK(world::load_level(b, text.data(), text.size()));  // a joiner gets the host's level
    editor::multi_user::Baseline base_a, base_b;
    editor::multi_user::capture(a, base_a);
    editor::multi_user::capture(b, base_b);
    CHECK(editor::multi_user::diff(a, base_a).empty());  // nothing changed yet

    // Move, rename, add a component, delete, create a child, change World Settings.
    a.get<world::Transform>(crate)->position = {1, 2, 3};
    a.set_name(floor, "Ground");
    a.add<world::PointLight>(lamp)->intensity = 42.0f;
    a.destroy(crate);
    const world::Entity child = a.create("Bulb");
    a.set_parent(child, lamp, false);
    a.settings().gravity = 1.62f;
    const std::string ops = editor::multi_user::diff(a, base_a);
    CHECK(!ops.empty() && ops.find("\"remove\"") != std::string::npos && ops.find("\"settings\"") != std::string::npos);
    CHECK(editor::multi_user::diff(a, base_a).empty());  // the baseline moved with it
    CHECK(editor::multi_user::apply(b, ops.data(), ops.size(), base_b));
    world::save_level(a, text);
    world::save_level(b, other);
    CHECK(text.size() == other.size() && std::memcmp(text.data(), other.data(), text.size()) == 0);
    CHECK(editor::multi_user::diff(b, base_b).empty());  // applied ops are not sent back

    // A component removed on the other side; then a bad message changes nothing it cannot parse.
    b.remove(lamp, reflect::type_of<world::PointLight>());
    const std::string back = editor::multi_user::diff(b, base_b);
    CHECK(editor::multi_user::apply(a, back.data(), back.size(), base_a));
    CHECK(!a.get<world::PointLight>(lamp) && a.parent(child) == lamp && std::strcmp(a.name(floor), "Ground") == 0);
    const char bad[] = "{\"set\": [{\"id\": oops";
    CHECK(!editor::multi_user::apply(a, bad, sizeof bad - 1, base_a));
}

// Background compiles: meshes not in DerivedData are queued (busy until a worker wrote them), then up to date;
// Compile All finds every one that is not compiled yet. Many at once, on the worker threads.
static void test_asset_compiler() {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(LSD_TEST_TEMP_DIR) / "compiler_test";
    fs::remove_all(root);
    fs::create_directories(root / "Content");
    constexpr int kMeshes = 40;
    for (int i = 0; i < kMeshes; ++i) {
        std::ofstream f(root / "Content" / ("SM_" + std::to_string(i) + ".obj"));
        f << "o SM_" << i << "\nv 0 0 0\nv 1 0 0\nv 1 1 " << i << "\nf 1 2 3\n";
    }
    HeapAllocator heap("compiler");
    asset::AssetRegistry registry(heap, (root / "Content").generic_string().c_str());
    registry.scan();
    const std::string data = (root / "DerivedData").generic_string();
    char key[33];
    asset::to_string(registry.get(0).guid, key);
    CHECK(editor::compiler::queue_mesh(registry, key, data.c_str()));  // not compiled: queued
    editor::compiler::wait();
    CHECK(!editor::compiler::mesh_busy(key, data.c_str()) && !editor::compiler::queue_mesh(registry, key, data.c_str()));  // done, up to date
    CHECK(editor::compiler::compile_all(registry, data.c_str(), false) == kMeshes - 1);  // the others
    editor::compiler::wait();
    const editor::compiler::Progress p = editor::compiler::progress();
    CHECK(p.done == p.total && p.failed == 0 && !editor::compiler::busy());
    CHECK(editor::compiler::compile_all(registry, data.c_str(), false) == 0);  // nothing left
    u32 files = 0, lod_files = 0;  // each compiled mesh with its generated LODs beside it (built on the workers too)
    for (const auto& entry : fs::directory_iterator(data)) {
        files += entry.path().extension() == ".lsdmesh" ? 1u : 0u;
        lod_files += entry.path().extension() == ".lod" ? 1u : 0u;
    }
    CHECK(files == kMeshes && lod_files == kMeshes);
    editor::compiler::shutdown();
    fs::remove_all(root);
}

// Button sounds: which elements count as clickable, and one hover sound per element entered.
static void test_ui_sounds() {
    using lsd::ui::is_clickable;
    CHECK(is_clickable("button", "", false, false, false));
    CHECK(is_clickable("select", "", false, false, false));
    CHECK(is_clickable("div", "", true, false, false));                    // onclick
    CHECK(is_clickable("div", "", false, true, false));                    // data-click-sound / data-hover-sound
    CHECK(is_clickable("input", "checkbox", false, false, false));
    CHECK(is_clickable("input", "submit", false, false, false));
    CHECK(!is_clickable("input", "text", false, false, false));            // a text field
    CHECK(!is_clickable("input", "range", false, false, false));
    CHECK(!is_clickable("div", "", false, false, false));
    CHECK(!is_clickable("span", "button", false, false, false));           // type means nothing outside <input>
    CHECK(!is_clickable("button", "", true, false, true));                 // disabled
    int a = 0, b = 0;
    lsd::ui::HoverTracker t;
    CHECK(!t.enter(&a, false));  // a document opened under a still cursor: silent
    CHECK(!t.enter(&a, true));   // moving inside it: still silent
    CHECK(!t.enter(nullptr, true));
    CHECK(t.enter(&a, true));    // back onto it: once
    CHECK(!t.enter(&a, true));
    CHECK(t.enter(&b, true));    // straight onto the next button
    CHECK(!t.enter(&b, false));
}

int main() {
    test_ui_sounds();
    test_asset_compiler();
    test_sound_synth();
    test_cloth_mesh();
    test_editor_utility();
    test_pcg_graph();
    test_paper2d();
    test_templates();
    test_scalability();
    test_device_profiles();
    test_run_behavior();
    test_state_tree_asset();
    test_gather_text();
    test_landscape_core();
    test_mesh_edit();
    test_tile_layers();
    test_keymap();
    test_landscape_paint();
    test_assistant();
    test_undo_redo();
    test_multi_user_ops();
    test_references();
    test_light_bake();
    test_dynamic_gi();
    test_lightmaps();
    test_hlod();
    test_rml_edit();
    test_rml_anchor();
    test_sound_cue();
    test_particle_light();
    test_shadow_cells();
    test_landscape_tiles();
    test_static_light_bake();
    test_push_helpers();
    test_bake_texture_albedo();
    test_post_blend();
    if (g_failures == 0) std::printf("All editor tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
