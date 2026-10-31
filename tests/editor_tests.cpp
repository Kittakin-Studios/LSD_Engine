// Editor tests that run without a GPU: undo / redo history and unsaved-change tracking.
#include "light_bake.h"
#include "references.h"

#include "lsd/asset/asset_registry.h"

#include <filesystem>
#include <fstream>
#include "rml_edit.h"
#include "sound_cue.h"
#include "transactions.h"

#include "lsd/world/components.h"

#include <cstdio>
#include <cstring>
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

static void test_rml_edit() {  // the UI designer's writes into the document text
    using namespace lsd::editor;
    std::string rml = "<rml><body>\n<div id=\"panel\" style=\"left: 40px; color: red;\">\n  <div id=\"title\">Title</div>\n  <button id=\"play\">Play</button>\n</div>\n<img id=\"pic\" src=\"a.png\"/>\n</body></rml>";
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
}

int main() {
    test_undo_redo();
    test_references();
    test_light_bake();
    test_rml_edit();
    test_sound_cue();
    if (g_failures == 0) std::printf("All editor tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
