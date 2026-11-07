// Editor tests that run without a GPU: undo / redo history and unsaved-change tracking.
#include "assistant_core.h"
#include "light_bake.h"
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
#include "lsd/world/level.h"

#include "lsd/world/components.h"

#include <algorithm>
#include <atomic>
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

int main() {
    test_paper2d();
    test_templates();
    test_scalability();
    test_run_behavior();
    test_gather_text();
    test_landscape_core();
    test_mesh_edit();
    test_tile_layers();
    test_keymap();
    test_landscape_paint();
    test_assistant();
    test_undo_redo();
    test_references();
    test_light_bake();
    test_lightmaps();
    test_rml_edit();
    test_sound_cue();
    if (g_failures == 0) std::printf("All editor tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
