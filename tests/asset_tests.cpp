// Asset pipeline tests, including the milestone 2 exit test on a real Blender FBX:
// import it into Content/, find it by search, compile it and load the result.
#include "lsd/asset/localization.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/texture.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/array.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

namespace fs = std::filesystem;
using namespace lsd;

static void test_guid_and_array() {
    const asset::Guid g = asset::new_guid();
    char text[33];
    asset::to_string(g, text);
    asset::Guid parsed;
    CHECK(std::strlen(text) == 32 && asset::parse_guid(text, parsed) && parsed == g);
    CHECK(!asset::parse_guid("not-a-guid", parsed));
    CHECK(asset::new_guid() != g);

    HeapAllocator heap("array");
    {
        Array<int> a(heap);
        for (int i = 0; i < 100; ++i) a.push_back(i);
        CHECK(a.size() == 100 && a[99] == 99);
        a.remove_swap(0);
        CHECK(a.size() == 99 && a[0] == 99);
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_blender_fbx(const fs::path& root) {
    HeapAllocator heap("assets");
    const fs::path content = root / "Content";
    const fs::path data = root / "DerivedData";
    fs::remove_all(root);
    fs::create_directories(content / "Props");
    fs::copy_file(fs::path(LSD_UFBX_DATA_DIR) / "blender_279_ball_7400_binary.fbx", content / "Props" / "Ball.fbx");

    asset::AssetRegistry registry(heap, content.generic_string().c_str());
    CHECK(registry.scan());
    CHECK(registry.count() == 1);
    const asset::AssetRecord& ball = registry.get(0);
    CHECK(ball.type == asset::AssetType::Mesh && ball.guid.valid());
    CHECK(std::strcmp(ball.name, "Ball") == 0 && std::strcmp(ball.folder, "Props") == 0 && std::strcmp(ball.path, "Props/Ball.fbx") == 0);
    CHECK(fs::exists(content / "Props" / "Ball.fbx.meta"));

    // Find it by search (exit test), case-insensitive; folder browsing; no false hits.
    usize hits[8];
    CHECK(registry.search("ball", asset::AssetType::Unknown, nullptr, hits, 8) == 1);
    CHECK(registry.search("BAL", asset::AssetType::Mesh, nullptr, hits, 8) == 1);
    CHECK(registry.search("", asset::AssetType::Unknown, "Props", hits, 8) == 1);
    CHECK(registry.search("", asset::AssetType::Unknown, "", hits, 8) == 0);
    CHECK(registry.search("chair", asset::AssetType::Unknown, nullptr, hits, 8) == 0);

    // The GUID survives a fresh registry (it lives in the .meta).
    const asset::Guid guid = ball.guid;
    asset::AssetRegistry again(heap, content.generic_string().c_str());
    again.scan();
    CHECK(again.count() == 1 && again.get(0).guid == guid);

    // Copying a file together with its .meta creates a new asset with a new GUID.
    fs::copy_file(content / "Props" / "Ball.fbx", content / "Props" / "Ball2.fbx");
    fs::copy_file(content / "Props" / "Ball.fbx.meta", content / "Props" / "Ball2.fbx.meta");
    again.scan();
    CHECK(again.count() == 2 && again.get(0).guid != again.get(1).guid);
    fs::remove(content / "Props" / "Ball2.fbx");
    CHECK(again.scan() && again.count() == 1);

    // Rename and move keep the GUID (the .meta travels with the file); bad names are refused.
    char error[256];
    CHECK(!asset::valid_name("a/b") && !asset::valid_name("a\\b") && !asset::valid_name("") && !asset::valid_name("..") && !asset::valid_name("x?") && asset::valid_name("Chair 01"));
    CHECK(!registry.rename_asset(registry.get(0), "bad:name", error, sizeof error) && error[0]);
    CHECK(registry.rename_asset(registry.get(0), "Orb", error, sizeof error));
    CHECK(registry.count() == 1 && registry.get(0).guid == guid && std::strcmp(registry.get(0).name, "Orb") == 0);
    CHECK(!fs::exists(content / "Props" / "Ball.fbx.meta") && fs::exists(content / "Props" / "Orb.fbx.meta"));
    fs::create_directories(content / "Shapes");
    CHECK(registry.move_asset(registry.get(0), "Shapes", error, sizeof error));
    CHECK(registry.count() == 1 && registry.get(0).guid == guid && std::strcmp(registry.get(0).folder, "Shapes") == 0);
    CHECK(!registry.move_asset(registry.get(0), "Missing", error, sizeof error));
    CHECK(registry.move_asset(registry.get(0), "Props", error, sizeof error) && registry.rename_asset(registry.get(0), "Ball", error, sizeof error));
    CHECK(registry.get(0).guid == guid);

    // Compile to DerivedData, load it back, check the Blender default UV sphere (radius 1 m).
    char compiled[300];
    CHECK(asset::compile_mesh_asset(registry, ball, data.generic_string().c_str(), heap, compiled, sizeof compiled));
    CHECK(fs::exists(compiled));
    render::MeshData mesh;
    asset::MeshBounds bounds;
    CHECK(asset::load_compiled_mesh(compiled, heap, mesh, &bounds));
    CHECK(mesh.vertex_count > 100 && mesh.index_count % 3 == 0 && mesh.index_count > mesh.vertex_count);
    for (int k = 0; k < 3; ++k) {
        CHECK(std::fabs(bounds.min[k] + 1.0f) < 0.05f);
        CHECK(std::fabs(bounds.max[k] - 1.0f) < 0.05f);
    }
    bool normals_unit = true;
    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        const f32* n = mesh.vertices[i].normal;
        normals_unit = normals_unit && std::fabs(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1.0f) < 1e-3f;
    }
    CHECK(normals_unit);
    render::free_mesh(heap, mesh);

    // Up to date: a second compile does not rewrite; a changed import scale does.
    const auto first_write = fs::last_write_time(compiled);
    CHECK(asset::compile_mesh_asset(registry, ball, data.generic_string().c_str(), heap, compiled, sizeof compiled));
    CHECK(fs::last_write_time(compiled) == first_write);
    asset::AssetMeta meta;
    CHECK(registry.load_meta(ball, meta));
    meta.import_scale = 2.0f;
    CHECK(registry.save_meta(ball, meta));
    CHECK(asset::compile_mesh_asset(registry, ball, data.generic_string().c_str(), heap, compiled, sizeof compiled));
    CHECK(asset::load_compiled_mesh(compiled, heap, mesh, &bounds));
    CHECK(std::fabs(bounds.max[2] - 2.0f) < 0.1f);
    render::free_mesh(heap, mesh);

    // Delete removes the file and its .meta.
    CHECK(registry.delete_asset(registry.get(0), error, sizeof error));
    CHECK(registry.count() == 0 && !fs::exists(content / "Props" / "Ball.fbx") && !fs::exists(content / "Props" / "Ball.fbx.meta"));
}

// A one-triangle glTF (Y-up) whose top vertex is at +Y must arrive pointing up (+Z).
static void test_gltf_axes(const fs::path& root) {
    HeapAllocator heap("gltf");
    // Positions (0,0,0) (1,0,0) (0,1,0) as 36 bytes of little-endian floats, base64.
    const char gltf[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]}]
})";
    fs::create_directories(root);
    const fs::path path = root / "triangle.gltf";
    FILE* f = std::fopen(path.string().c_str(), "wb");
    std::fwrite(gltf, 1, sizeof gltf - 1, f);
    std::fclose(f);
    render::MeshData mesh;
    char error[256];
    CHECK(asset::import_mesh(path.string().c_str(), 1.0f, heap, mesh, error, sizeof error));
    if (mesh.vertex_count == 3) {
        const asset::MeshBounds b = asset::compute_bounds(mesh);
        CHECK(std::fabs(b.max[2] - 1.0f) < 1e-5f && std::fabs(b.max[1]) < 1e-5f);  // +Y became +Z
    } else {
        CHECK(mesh.vertex_count == 3);
    }
    render::free_mesh(heap, mesh);
    CHECK(!asset::import_mesh((root / "missing.fbx").string().c_str(), 1.0f, heap, mesh, error, sizeof error) && error[0]);
}

// Textures: mip chain sizes, colour mips averaged in light (sRGB) vs data mips in bytes, size cap, TGA load.
static void test_texture(const fs::path& root) {
    // 4x2: left half black, right half white.
    u8 pixels[4 * 2 * 4];
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const u8 v = x < 2 ? 0 : 255;
            u8* p = pixels + (y * 4 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    asset::TextureData t(default_allocator());
    CHECK(asset::build_texture(pixels, 4, 2, true, 4096, t));
    CHECK(t.width == 4 && t.height == 2 && t.mip_count == 3);  // 4x2, 2x1, 1x1
    CHECK(t.mip_offset[1] == 4 * 2 * 4 && t.mip_offset[2] == t.mip_offset[1] + 2 * 4 && t.pixels.size() == t.mip_offset[2] + 4);
    CHECK(t.mip(1)[0] == 0 && t.mip(1)[4] == 255);  // 2x1: still black | white
    CHECK(t.mip(2)[0] >= 186 && t.mip(2)[0] <= 189);  // half the light of white is sRGB 188, not 128
    CHECK(t.mip(2)[3] == 255);
    asset::TextureData data(default_allocator());
    CHECK(asset::build_texture(pixels, 4, 2, false, 4096, data) && data.mip(2)[0] == 128);  // data: plain average
    asset::TextureData capped(default_allocator());
    CHECK(asset::build_texture(pixels, 4, 2, true, 2, capped) && capped.width == 2 && capped.height == 1 && capped.mip_count == 2);
    CHECK(!asset::build_texture(nullptr, 4, 2, true, 4096, t));

    // A real file through stb_image: an uncompressed 2x2 TGA (red, green / blue, white).
    fs::create_directories(root);
    const fs::path tga = root / "checker.tga";
    const u8 header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28};  // 32-bit, top-left origin
    const u8 bgra[16] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    FILE* f = std::fopen(tga.string().c_str(), "wb");
    std::fwrite(header, 1, sizeof header, f);
    std::fwrite(bgra, 1, sizeof bgra, f);
    std::fclose(f);
    asset::TextureData loaded(default_allocator());
    CHECK(asset::load_texture(tga.string().c_str(), false, 4096, loaded));
    CHECK(loaded.width == 2 && loaded.height == 2 && loaded.mip_count == 2);
    CHECK(loaded.mip(0)[0] == 255 && loaded.mip(0)[1] == 0 && loaded.mip(0)[5] == 255 && loaded.mip(0)[10] == 255);  // red, green, blue
    CHECK(!asset::load_texture((root / "missing.png").string().c_str(), true, 4096, loaded));
    CHECK(asset::type_from_extension(".PNG") == asset::AssetType::Texture && asset::type_from_extension(".tga") == asset::AssetType::Texture);
}

static void test_localization() {
    const char* path = LSD_TEST_TEMP_DIR "/ST_Test.csv";
    if (FILE* f = std::fopen(path, "wb")) {
        std::fputs("Key,en,fr\nMENU_PLAY,Play,Jouer\nMENU_QUIT,Quit,\n", f);
        std::fclose(f);
    }
    asset::Localization loc(default_allocator());
    CHECK(loc.load(path));
    CHECK(loc.language_count() == 2 && std::strcmp(loc.language_code(1), "fr") == 0);
    CHECK(std::strcmp(loc.text("MENU_PLAY"), "Play") == 0);
    CHECK(loc.set_language("fr") && std::strcmp(loc.text("MENU_PLAY"), "Jouer") == 0);
    CHECK(std::strcmp(loc.text("MENU_QUIT"), "Quit") == 0);  // untranslated: the first language
    CHECK(std::strcmp(loc.text("NOPE"), "NOPE") == 0 && !loc.find("NOPE"));
    CHECK(!loc.set_language("de") && std::strcmp(loc.text("MENU_PLAY"), "Play") == 0);
}

int main() {
    test_localization();
    const fs::path root = fs::path(LSD_TEST_TEMP_DIR) / "asset_tests";
    test_guid_and_array();
    test_blender_fbx(root);
    test_gltf_axes(root);
    test_texture(root);
    if (g_failures == 0) std::printf("All asset tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
