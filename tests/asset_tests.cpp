// Asset pipeline tests, including the milestone 2 exit test on a real Blender FBX:
// import it into Content/, find it by search, compile it and load the result.
#include "lsd/asset/asset_meta.h"
#include "lsd/asset/localization.h"
#include "lsd/asset/light_profile.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/texture.h"
#include "lsd/asset/material_import.h"
#include "lsd/asset/plugins.h"
#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/array.h"

#include <tinyexr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>

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
    // The generation changes only when the records do (panels holding indices check it).
    const u32 generation = registry.generation();
    registry.scan();
    CHECK(registry.generation() == generation);
    fs::copy_file(content / "Props" / "Ball.fbx", content / "Props" / "Ball2.fbx");
    registry.scan();
    CHECK(registry.generation() != generation && registry.count() == 2);
    fs::remove(content / "Props" / "Ball2.fbx");
    fs::remove(content / "Props" / "Ball2.fbx.meta");
    registry.scan();
    CHECK(registry.count() == 1);

    // Find it by search (exit test), case-insensitive; folder browsing; no false hits.
    usize hits[8];
    CHECK(registry.search("ball", asset::AssetType::Unknown, nullptr, hits, 8) == 1);
    CHECK(registry.search("BAL", asset::AssetType::Mesh, nullptr, hits, 8) == 1);
    CHECK(registry.search("", asset::AssetType::Unknown, "Props", hits, 8) == 1);
    CHECK(registry.search("", asset::AssetType::Unknown, "", hits, 8) == 0);
    CHECK(registry.search("chair", asset::AssetType::Unknown, nullptr, hits, 8) == 0);

    // Tags in the .meta are searchable; a later async walk picks up a new file.
    {
        asset::AssetMeta meta;
        CHECK(registry.load_meta(registry.get(0), meta));
        std::snprintf(meta.tags, sizeof meta.tags, "sports, round");
        CHECK(registry.save_meta(registry.get(0), meta));
        CHECK(registry.search("ROUND", asset::AssetType::Unknown, nullptr, hits, 8) == 1);
        asset::AssetRegistry tagged(heap, content.generic_string().c_str());
        CHECK(tagged.scan() && std::strcmp(tagged.get(0).tags, "sports, round") == 0);
        fs::copy_file(content / "Props" / "Ball.fbx", content / "Props" / "Async.fbx");
        CHECK(!tagged.scan_async());  // starts the walk
        bool applied = false;
        for (int tries = 0; tries < 500 && !applied; ++tries) {
            applied = tagged.scan_async();
            if (!applied) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(applied && tagged.count() == 2);
        fs::remove(content / "Props" / "Async.fbx");
        fs::remove(content / "Props" / "Async.fbx.meta");
    }

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
    CHECK(mesh.vertex_count == 42 && mesh.index_count == 240);  // an 80-face icosphere: smooth by default welds it to its 42 points
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

    // Parts: two nodes share the triangle (the second 5 m up glTF's Y, so +Z), the second with two
    // materials; each part imports alone, in its node's space, with the node's transform beside it.
    const char parts_gltf[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0, "scenes": [{"nodes": [0, 2]}],
  "nodes": [{"name": "Base", "mesh": 0}, {"name": "Top", "mesh": 1, "translation": [0, 5, 0]}, {"name": "Group", "children": [1]}],
  "materials": [{"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1], "metallicFactor": 0, "roughnessFactor": 0.25}},
                {"name": "Gold", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 1, "metallicRoughnessTexture": {"index": 1}},
                 "normalTexture": {"index": 2}}],
  "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
  "images": [{"uri": "textures/Gold_Base.png"}, {"uri": "Gold_MR.png"}, {"uri": "textures\\Gold_N.png"}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]},
             {"primitives": [{"attributes": {"POSITION": 0}, "material": 0}, {"attributes": {"POSITION": 0}, "material": 1}]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]}]
})";
    const fs::path parts_path = root / "parts.gltf";
    f = std::fopen(parts_path.string().c_str(), "wb");
    std::fwrite(parts_gltf, 1, sizeof parts_gltf - 1, f);
    std::fclose(f);
    Array<asset::MeshPart> parts(heap);
    CHECK(asset::list_mesh_parts(parts_path.string().c_str(), parts, error, sizeof error) && parts.size() == 3);
    if (parts.size() == 3) {
        CHECK(std::strcmp(parts[0].name, "Base") == 0 && !parts[0].material[0]);
        CHECK(std::strcmp(parts[1].name, "Top_Red") == 0 && parts[1].base_color[1] == 0.0f && parts[1].roughness == 0.25f);
        CHECK(std::strcmp(parts[2].name, "Top_Gold") == 0 && parts[2].metallic == 1.0f && std::strcmp(parts[2].texture, "Gold_Base.png") == 0);
        CHECK(std::strcmp(parts[2].orm_texture, "Gold_MR.png") == 0 && std::strcmp(parts[2].normal_texture, "Gold_N.png") == 0);
        CHECK(std::strcmp(parts[0].node_path, "Base") == 0 && std::strcmp(parts[1].node_path, "Group/Top") == 0 && std::strcmp(parts[2].node_path, "Group/Top") == 0);  // the node tree
    }
    CHECK(asset::import_mesh(parts_path.string().c_str(), 1.0f, heap, mesh, error, sizeof error, 2) && mesh.vertex_count == 3);
    CHECK(std::fabs(asset::compute_bounds(mesh).min[2]) < 1e-5f && std::fabs(asset::compute_bounds(mesh).max[2] - 1.0f) < 1e-5f);  // its node's space...
    if (parts.size() == 3) CHECK(std::fabs(parts[2].position.z - 5.0f) < 1e-5f && std::fabs(parts[2].position.y) < 1e-5f && std::fabs(parts[0].position.z) < 1e-5f);  // ...placed 5 m up
    render::free_mesh(heap, mesh);
    CHECK(!asset::import_mesh(parts_path.string().c_str(), 1.0f, heap, mesh, error, sizeof error, 3) && error[0]);

    // Mesh keys: a GUID, or a GUID and a part; nothing else.
    asset::Guid guid;
    i32 part = 0;
    CHECK(asset::parse_mesh_key("0123456789abcdef0123456789abcdef", guid, part) && part == -1);
    CHECK(asset::parse_mesh_key("0123456789abcdef0123456789abcdef#12", guid, part) && part == 12);
    CHECK(!asset::parse_mesh_key("0123456789abcdef0123456789abcdef#", guid, part));
    CHECK(!asset::parse_mesh_key("0123456789abcdef0123456789abcdef#1x", guid, part));
    CHECK(!asset::parse_mesh_key("Cube", guid, part));
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
    // BC compression: an opaque 8x8 becomes BC1 (8 bytes a 4x4 block, every mip at least one block); with alpha, BC3.
    {
        std::vector<u8> rgba(8 * 8 * 4, 255);
        for (usize i = 0; i < rgba.size(); i += 4) rgba[i] = static_cast<u8>(i);  // a ramp in red
        asset::TextureData bc(default_allocator());
        CHECK(asset::build_texture(rgba.data(), 8, 8, true, 4096, bc) && bc.mip_count == 4);
        CHECK(asset::compress_texture(bc) && bc.encoding == asset::TextureEncoding::BC1);
        CHECK(bc.mip_offset[1] == 4 * 8 && bc.mip_offset[2] == 5 * 8 && bc.mip_offset[3] == 6 * 8 && bc.pixels.size() == 7 * 8);  // 2x2 + 1 + 1 + 1 blocks
        CHECK(!asset::compress_texture(bc));  // already compressed
        rgba[3] = 0;  // one see-through pixel
        asset::TextureData a(default_allocator());
        CHECK(asset::build_texture(rgba.data(), 8, 8, true, 4096, a) && asset::compress_texture(a) && a.encoding == asset::TextureEncoding::BC3 && a.mip_offset[1] == 4 * 16);
        {  // a normal map: BC5, X and Y kept (a flat one: both 128, its two endpoints in each channel's 8 bytes)
            std::vector<u8> flat(8 * 8 * 4);
            for (usize i = 0; i < flat.size(); i += 4) { flat[i] = 128; flat[i + 1] = 128; flat[i + 2] = 255; flat[i + 3] = 255; }
            asset::TextureData nm(default_allocator());
            CHECK(asset::build_texture(flat.data(), 8, 8, false, 4096, nm) && asset::compress_texture(nm, true) && nm.encoding == asset::TextureEncoding::BC5);
            CHECK(nm.mip_offset[1] == 4 * 16 && nm.pixels[0] == 128 && nm.pixels[8] == 128);
        }
        // Cached: the first load encodes and writes the file, the second reads it back the same; a newer source is encoded again.
        const fs::path cache = root / "TexCache";
        fs::remove_all(cache);
        asset::TextureData first(default_allocator()), again(default_allocator()), plain(default_allocator());
        CHECK(asset::load_texture_cached(tga.string().c_str(), true, 4096, true, cache.string().c_str(), plain) && plain.encoding == asset::TextureEncoding::RGBA8);  // 2x2: too small to compress
        const fs::path big = root / "big.png";
        CHECK(asset::write_png(big.string().c_str(), 8, 8, rgba.data()));
        CHECK(asset::load_texture_cached(big.string().c_str(), true, 4096, true, cache.string().c_str(), first) && first.encoding == asset::TextureEncoding::BC3);
        CHECK(std::distance(fs::directory_iterator(cache), fs::directory_iterator{}) == 2);
        CHECK(asset::load_texture_cached(big.string().c_str(), true, 4096, true, cache.string().c_str(), again) && again.encoding == first.encoding && again.pixels.size() == first.pixels.size() &&
              std::memcmp(again.pixels.data(), first.pixels.data(), first.pixels.size()) == 0 && again.mip_offset[2] == first.mip_offset[2]);
        fs::remove_all(cache);
        {  // float textures: an HDR image kept as half floats, a value of 4 stays 4 (not clipped to 1)
            std::vector<f32> bright(4 * 4 * 4, 4.0f);
            const fs::path hdr = root / "bright.exr";
            const char* exr_error = nullptr;
            CHECK(SaveEXR(bright.data(), 4, 4, 4, 0, hdr.string().c_str(), &exr_error) == TINYEXR_SUCCESS);
            asset::TextureData half(default_allocator()), clipped(default_allocator());
            CHECK(asset::load_texture(hdr.string().c_str(), false, 4096, half, true) && half.encoding == asset::TextureEncoding::RGBA16F && half.mip_count == 3);
            u16 h0;
            std::memcpy(&h0, half.mip(0), 2);
            CHECK(h0 == 0x4400);  // 4.0 in half
            CHECK(half.mip_offset[1] == 4 * 4 * 8);
            CHECK(asset::load_texture(hdr.string().c_str(), false, 4096, clipped) && clipped.encoding == asset::TextureEncoding::RGBA8 && clipped.mip(0)[0] == 255);
        }
        asset::TextureData odd(default_allocator());
        CHECK(asset::build_texture(rgba.data(), 6, 6, true, 4096, odd) && !asset::compress_texture(odd) && odd.encoding == asset::TextureEncoding::RGBA8);  // not a multiple of 4
    }
    Array<u8> png(default_allocator());  // a picture for the assistant: PNG bytes in memory, halved to fit
    CHECK(asset::encode_png(tga.string().c_str(), 1, png) && png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    CHECK(!asset::encode_png((root / "missing.png").string().c_str(), 64, png));
    CHECK(asset::type_from_extension(".PNG") == asset::AssetType::Texture && asset::type_from_extension(".tga") == asset::AssetType::Texture);

    // Float images: a 2x1 Radiance .hdr (flat RGBE: 1, 0.5, 0.25 and 4, 0, 0) and an EXR written by tinyexr.
    CHECK(asset::type_from_extension(".hdr") == asset::AssetType::Texture && asset::type_from_extension(".EXR") == asset::AssetType::Texture);
    const fs::path hdr = root / "ramp.hdr";
    f = std::fopen(hdr.string().c_str(), "wb");
    const char hdr_header[] = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    const u8 rgbe[8] = {128, 64, 32, 129, 128, 0, 0, 131};
    std::fwrite(hdr_header, 1, sizeof hdr_header - 1, f);
    std::fwrite(rgbe, 1, sizeof rgbe, f);
    std::fclose(f);
    CHECK(asset::load_texture(hdr.string().c_str(), false, 4096, loaded) && loaded.width == 2 && loaded.height == 1);
    CHECK(loaded.mip(0)[0] == 255 && loaded.mip(0)[1] == 128 && loaded.mip(0)[2] == 64 && loaded.mip(0)[4] == 255 && loaded.mip(0)[5] == 0);  // linear; 4 clipped to 1
    CHECK(asset::load_texture(hdr.string().c_str(), true, 4096, loaded) && loaded.mip(0)[1] >= 186 && loaded.mip(0)[1] <= 189);  // 0.5 linear as sRGB
    const fs::path exr = root / "ramp.exr";
    const float exr_pixels[2 * 2 * 4] = {0.25f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f};
    const char* err = nullptr;
    CHECK(SaveEXR(exr_pixels, 2, 2, 4, 1, exr.string().c_str(), &err) == TINYEXR_SUCCESS);
    u32 ew = 0, eh = 0;
    CHECK(asset::texture_size(exr.string().c_str(), ew, eh) && ew == 2 && eh == 2);  // from the header
    CHECK(asset::load_texture(exr.string().c_str(), false, 4096, loaded) && loaded.width == 2 && loaded.height == 2);
    CHECK(loaded.mip(0)[0] == 64 && loaded.mip(0)[1] == 128 && loaded.mip(0)[2] == 255 && loaded.mip(0)[8] == 255 && loaded.mip(0)[11] == 128);  // 0.25, 0.5, 1; 2 clipped; alpha 0.5
}

// Animation curves: straight between keys given in any order, held past the ends, per clip and name.
static void test_anim_curves() {
    asset::AssetMeta meta;
    auto key = [&](const char* curve, u32 clip, f32 time, f32 value) {
        asset::AnimCurveKey k;
        std::snprintf(k.curve, sizeof k.curve, "%s", curve);
        k.clip = clip;
        k.time = time;
        k.value = value;
        meta.curve_keys.push_back(k);
    };
    key("Footstep", 0, 1.0f, 1.0f);
    key("Footstep", 0, 0.0f, 0.0f);
    key("Footstep", 1, 0.0f, 5.0f);
    key("Trail", 0, 0.5f, 2.0f);
    f32 v = -1.0f;
    CHECK(asset::curve_value(meta, 0, "Footstep", 0.25f, v) && std::fabs(v - 0.25f) < 1e-6f);
    CHECK(asset::curve_value(meta, 0, "Footstep", 3.0f, v) && v == 1.0f);   // held after the last key
    CHECK(asset::curve_value(meta, 0, "Footstep", -1.0f, v) && v == 0.0f);  // and before the first
    CHECK(asset::curve_value(meta, 1, "Footstep", 0.7f, v) && v == 5.0f);   // another clip's own keys
    CHECK(asset::curve_value(meta, 0, "Trail", 0.0f, v) && v == 2.0f);      // one key: everywhere
    CHECK(!asset::curve_value(meta, 2, "Footstep", 0.0f, v) && !asset::curve_value(meta, 0, "Nope", 0.0f, v));
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

    // Arguments and plurals (FText::Format): English 1 is "one"; French 0 and 1 are; zero= wins at 0.
    char out[64];
    const char* one[] = {"1"};
    const char* three[] = {"3", "Ada"};
    const char* none[] = {"0"};
    const char* apples = "{0} {0}|plural(one=apple,other=apples)";
    asset::format_text(apples, three, 2, "en", out, sizeof out);
    CHECK(std::strcmp(out, "3 apples") == 0);
    asset::format_text(apples, one, 1, "en", out, sizeof out);
    CHECK(std::strcmp(out, "1 apple") == 0);
    asset::format_text(apples, none, 1, "en", out, sizeof out);
    CHECK(std::strcmp(out, "0 apples") == 0);
    asset::format_text("{0} {0}|plural(one=pomme,other=pommes)", none, 1, "fr", out, sizeof out);
    CHECK(std::strcmp(out, "0 pomme") == 0);
    asset::format_text("{0}|plural(zero=No mail,one=One mail,other={1} has mail)", none, 1, "en", out, sizeof out);
    CHECK(std::strcmp(out, "No mail") == 0);
    // few / many (CLDR): Russian, Polish, Arabic, Japanese.
    auto plural = [&](const char* n, const char* language, const char* pattern) {
        const char* args[] = {n};
        asset::format_text(pattern, args, 1, language, out, sizeof out);
        return std::string(out);
    };
    const char* ru = "{0}|plural(one=one,few=few,many=many,other=other)";
    CHECK(plural("1", "ru", ru) == "one" && plural("21", "ru", ru) == "one" && plural("3", "ru", ru) == "few" && plural("24", "ru", ru) == "few");
    CHECK(plural("5", "ru", ru) == "many" && plural("11", "ru", ru) == "many" && plural("12", "ru", ru) == "many" && plural("1.5", "ru", ru) == "other");
    CHECK(plural("22", "pl", ru) == "few" && plural("25", "pl", ru) == "many" && plural("21", "pl", ru) == "many" && plural("1", "pl", ru) == "one");
    const char* ar = "{0}|plural(zero=z,one=o,two=t,few=f,many=m,other=x)";
    CHECK(plural("0", "ar", ar) == "z" && plural("2", "ar", ar) == "t" && plural("7", "ar", ar) == "f" && plural("11", "ar", ar) == "m" && plural("100", "ar", ar) == "x");
    CHECK(plural("1", "ja", ru) == "other" && plural("3", "ru", "{0}|plural(one=a,other=b)") == "b");  // no form: other; a missing form: other
    CHECK(plural("1", "en-GB", ru) == "one" && plural("3", "cs", ru) == "few");
    asset::format_text("Hi {1}, {0} new; {5} {x}", three, 2, "en", out, sizeof out);
    CHECK(std::strcmp(out, "Hi Ada, 3 new;  {x}") == 0);  // missing argument: empty; not an argument: as written
    asset::format_text("{1} wins", three, 2, "en", out, 6);
    CHECK(std::strcmp(out, "Ada w") == 0);  // cut to fit
}

// Normal modes on two faces meeting at a right angle along the edge x = 0..1 (y = z = 0): a floor
// (normal +Z) and a wall (normal +Y, facing the floor: an inside corner). The edge's corners carry different UVs (a seam) on purpose.
static void test_normals() {
    HeapAllocator heap("normals");
    auto make = [](rhi::MeshVertex* v) {
        const f32 p[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},    // floor: (0,0,0) (1,0,0) (0,1,0)
                             {1, 0, 0}, {0, 0, 0}, {0, 0, 1}};   // wall:  (1,0,0) (0,0,0) (0,0,1)
        for (int i = 0; i < 6; ++i) {
            v[i] = rhi::MeshVertex{{p[i][0], p[i][1], p[i][2]}, {0, 0, 1}, {static_cast<f32>(i), 0}};
        }
    };
    rhi::MeshVertex v[6];
    auto near = [](const rhi::MeshVertex& m, f32 x, f32 y, f32 z) { return std::fabs(m.normal[0] - x) + std::fabs(m.normal[1] - y) + std::fabs(m.normal[2] - z) < 1e-4f; };
    make(v);
    asset::compute_normals(v, 6, asset::kNormalsFlat, 30.0f, heap);
    CHECK(near(v[0], 0, 0, 1) && near(v[4], 0, 1, 0));
    make(v);
    asset::compute_normals(v, 6, asset::kNormalsSmooth, 30.0f, heap);
    const f32 h = 0.70710678f;
    CHECK(near(v[0], 0, h, h) && near(v[4], 0, h, h) && near(v[2], 0, 0, 1));  // the shared edge blends, across the UV seam
    make(v);
    asset::compute_normals(v, 6, asset::kNormalsAutoSmooth, 30.0f, heap);
    CHECK(near(v[0], 0, 0, 1) && near(v[4], 0, 1, 0));  // 90 degrees > 30: kept hard
    make(v);
    asset::compute_normals(v, 6, asset::kNormalsAutoSmooth, 100.0f, heap);
    CHECK(near(v[0], 0, h, h));  // under 100: smoothed
    make(v);
    asset::compute_normals(v, 6, asset::kNormalsImport, 30.0f, heap);
    CHECK(near(v[4], 0, 0, 1));  // left alone
}

// A real FBX with nested, rotated and scaled nodes: its parts, each in its node's space and placed by
// the node's transform, cover the same box as the merged import.
static void test_fbx_parts() {
    HeapAllocator heap("parts");
    const std::string path = std::string(LSD_UFBX_DATA_DIR) + "/blender_279_nested_meshes_7400_binary.fbx";
    char error[256];
    Array<asset::MeshPart> parts(heap);
    CHECK(asset::list_mesh_parts(path.c_str(), parts, error, sizeof error) && parts.size() > 1);
    render::MeshData whole;
    CHECK(asset::import_mesh(path.c_str(), 1.0f, heap, whole, error, sizeof error));
    const asset::MeshBounds expected = asset::compute_bounds(whole);
    render::free_mesh(heap, whole);
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (usize k = 0; k < parts.size(); ++k) {
        render::MeshData part;
        CHECK(asset::import_mesh(path.c_str(), 1.0f, heap, part, error, sizeof error, static_cast<i32>(k)));
        const Mat4 m = trs(parts[k].position, parts[k].rotation, parts[k].scale);
        for (u32 i = 0; i < part.vertex_count; ++i) {
            const Vec3 p = transform_point(m, {part.vertices[i].position[0], part.vertices[i].position[1], part.vertices[i].position[2]});
            lo = vmin(lo, p);
            hi = vmax(hi, p);
        }
        render::free_mesh(heap, part);
    }
    std::printf("  %zu parts; merged box %.3f..%.3f, placed parts %.3f..%.3f (x)\n", parts.size(), static_cast<double>(expected.min[0]),
                static_cast<double>(expected.max[0]), static_cast<double>(lo.x), static_cast<double>(hi.x));
    const Vec3 elo{expected.min[0], expected.min[1], expected.min[2]}, ehi{expected.max[0], expected.max[1], expected.max[2]};
    CHECK(length(lo - elo) < 1e-3f && length(hi - ehi) < 1e-3f);
}

// Import Materials: a Blender FBX's materials with their maps (embedded images readable), and the
// channel packing that builds ORM / colour + opacity textures from maps of different sizes.
static void test_material_import(const fs::path& root) {
    HeapAllocator heap("materials");
    char error[256];
    {
        asset::ImportedMaterials file(heap);
        const std::string path = std::string(LSD_UFBX_DATA_DIR) + "/blender_293_embedded_textures_7400_binary.fbx";
        CHECK(asset::list_mesh_materials(path.c_str(), file, error, sizeof error) && !file.materials.empty());
        u32 embedded = 0;
        for (const asset::ImportedMaterial& m : file.materials) {
            CHECK(m.name[0]);
            for (const asset::ImportedMap* map : {&m.base_color_map, &m.normal_map, &m.metallic_map, &m.roughness_map, &m.occlusion_map, &m.opacity_map}) {
                if (!map->embedded) continue;
                asset::Image image;
                CHECK(asset::load_image(map->embedded, map->embedded_size, image) && image.width > 0);
                ++embedded;
            }
        }
        std::printf("  %zu materials, %u embedded maps\n", file.materials.size(), embedded);
        CHECK(embedded > 0);
        asset::ImportedMaterials none(heap);
        CHECK(!asset::list_mesh_materials("missing.fbx", none, error, sizeof error));
    }
    {  // glTF alphaMode / alphaCutoff / doubleSided reach the import (Masked or Translucent Materials)
        fs::create_directories(root);
        const fs::path gltf = root / "alpha.gltf";
        std::ofstream(gltf) << R"({"asset": {"version": "2.0"}, "materials": [{"name": "Glass", "alphaMode": "BLEND"},
            {"name": "Leaves", "alphaMode": "MASK", "alphaCutoff": 0.25, "doubleSided": true}, {"name": "Stone"}]})";
        asset::ImportedMaterials file(heap);
        CHECK(asset::list_mesh_materials(gltf.string().c_str(), file, error, sizeof error) && file.materials.size() == 3);
        CHECK(file.materials[0].alpha_mode == 2 && !file.materials[0].double_sided);
        CHECK(file.materials[1].alpha_mode == 1 && std::fabs(file.materials[1].alpha_cutoff - 0.25f) < 1e-6f && file.materials[1].double_sided);
        CHECK(file.materials[2].alpha_mode == 0);
        fs::remove(gltf);
    }
    CHECK(heap.allocated_bytes() == 0);
    // A 2x2 map and a 4x4 map packed: the output takes the larger size, the small one sampled by UV.
    auto make = [](asset::Image& image, u32 size, u8 base) {
        image.width = image.height = size;
        image.pixels = static_cast<u8*>(std::malloc(size * size * 4));  // freed by Image (stb frees with free)
        for (u32 i = 0; i < size * size; ++i) {
            for (u32 c = 0; c < 4; ++c) image.pixels[i * 4 + c] = static_cast<u8>(base + i * 10 + c);
        }
    };
    asset::Image small, big;
    make(small, 2, 0);
    make(big, 4, 100);
    fs::create_directories(root);
    const std::string out = (root / "packed.png").string();
    asset::PackSource r{&small, 0}, g{&big, 1}, b{nullptr, 0, 200}, a{&big, 2, 255, true};
    CHECK(asset::pack_channels(out.c_str(), r, g, b, a));
    asset::Image packed;
    CHECK(asset::load_image(out.c_str(), packed) && packed.width == 4 && packed.height == 4);
    if (packed.pixels) {
        const u8* p = &packed.pixels[(1 * 4 + 3) * 4];  // x 3, y 1: small's texel (1, 0), big's texel 7
        CHECK(p[0] == 10 && p[1] == 100 + 70 + 1 && p[2] == 200 && p[3] == 255 - (100 + 70 + 2));
    }
}

// Unreal-style texture names (one map per file) become one Material per base name.
static void test_map_names() {
    using namespace lsd;
    using asset::MapRole;
    struct Case { const char* stem; const char* base; MapRole role; };
    const Case cases[] = {
        {"MI_ClassicMotorBike01_Body01_BaseColor", "MI_ClassicMotorBike01_Body01", MapRole::BaseColor},
        {"MI_ClassicMotorBike01_Body01_Normal", "MI_ClassicMotorBike01_Body01", MapRole::Normal},
        {"MI_ClassicMotorBike01_Body01_Roughness", "MI_ClassicMotorBike01_Body01", MapRole::Roughness},
        {"MI_ClassicMotorBike01_Body01_Metallic", "MI_ClassicMotorBike01_Body01", MapRole::Metallic},
        {"MI_ClassicMotorBike01_Body01_AmbientOcclusion", "MI_ClassicMotorBike01_Body01", MapRole::Occlusion},
        {"MI_ClassicMotorBike01_Lights_EmissiveColor", "MI_ClassicMotorBike01_Lights", MapRole::Emissive},
        {"MI_ClassicMotorBike01_Lights_Opacity", "MI_ClassicMotorBike01_Lights", MapRole::Opacity},
        {"mi_bike_body02_SPECULAR", "mi_bike_body02", MapRole::Specular},
        {"Rock-albedo", "Rock", MapRole::BaseColor},
        {"Rock-nrm", "Rock", MapRole::Normal},
        {"Rock_ARM", "Rock", MapRole::Orm},
        {"Rock_OcclusionRoughnessMetallic", "Rock", MapRole::Orm},
        {"Rock_MetallicRoughness", "Rock", MapRole::MetallicRoughness},
        {"Rock_Displacement", "Rock", MapRole::Height},
        {"Rock_Smoothness", "Rock", MapRole::Glossiness},
        {"T_Brick_D", "T_Brick", MapRole::BaseColor},
        {"T_Brick_N", "T_Brick", MapRole::Normal},
        {"T_Brick_R", "T_Brick", MapRole::Roughness},
        {"T_Brick_M", "T_Brick", MapRole::Metallic},
        {"T_Brick_B", "T_Brick", MapRole::BaseColor},
        {"Door", "Door", MapRole::BaseColor},      // no suffix: a colour map named as a whole
        {"Backdrop", "Backdrop", MapRole::BaseColor},  // "drop" is not a suffix: nothing before it is cut
    };
    for (const Case& c : cases) {
        const asset::MapName n = asset::classify_map_name(c.stem);
        if (std::strcmp(n.base, c.base) != 0 || n.role != c.role) std::printf("  %s -> %s / %s\n", c.stem, n.base, asset::map_role_name(n.role));
        CHECK(std::strcmp(n.base, c.base) == 0 && n.role == c.role);
    }
    const char* stems[] = {"MI_Bike_Body01_BaseColor", "MI_Bike_Body02_BaseColor", "MI_Bike_Body01_Normal", "mi_bike_body01_roughness", "MI_Bike_Body02_Normal",
                           "MI_Bike_Body01_AmbientOcclusion", "MI_Bike_Lights_EmissiveColor"};
    std::vector<asset::MapSet> sets;
    asset::group_map_names(stems, 7, sets);
    CHECK(sets.size() == 3);
    CHECK(sets[0].map[static_cast<u32>(MapRole::BaseColor)] == 0 && sets[0].map[static_cast<u32>(MapRole::Normal)] == 2 && sets[0].map[static_cast<u32>(MapRole::Roughness)] == 3);
    CHECK(sets[0].map[static_cast<u32>(MapRole::Occlusion)] == 5 && sets[0].map[static_cast<u32>(MapRole::Metallic)] == -1);
    CHECK(sets[1].map[static_cast<u32>(MapRole::BaseColor)] == 1 && sets[1].map[static_cast<u32>(MapRole::Normal)] == 4);
    CHECK(sets[2].map[static_cast<u32>(MapRole::Emissive)] == 6);
}

// IES: candela averaged around the axis, normalised, resampled every 12 degrees; nothing outside the file's angles.
static void test_ies() {
    using namespace lsd;
    const char* ies = "IESNA:LM-63-2002\n[TEST] spot\nTILT=NONE\n1 1000 1 4 2 1 2 0.1 0.1 0\n1.0 1.0 50\n0 30 60 90\n0 90\n"
                      "1000 800 200 0\n1000, 600, 200, 0\n";
    f32 p[asset::kLightProfileSamples];
    CHECK(asset::parse_ies(ies, p));
    CHECK(std::fabs(p[0] - 1.0f) < 1e-4f);
    CHECK(std::fabs(p[1] - 0.88f) < 1e-4f);   // 12 degrees: 1000 -> 700 over 30
    CHECK(std::fabs(p[5] - 0.2f) < 1e-4f);    // 60 degrees
    CHECK(p[15] == 0.0f);                     // straight behind: outside 0..90
    CHECK(std::fabs(asset::light_profile_at(p, 1.0f) - 1.0f) < 1e-4f);
    CHECK(std::fabs(asset::light_profile_at(p, std::cos(6.0f * 3.14159265f / 180.0f)) - 0.94f) < 1e-3f);
    CHECK(!asset::parse_ies("not a profile", p));
}

static void write_text(const fs::path& file, const char* text) {
    fs::create_directories(file.parent_path());
    if (FILE* f = std::fopen(file.string().c_str(), "wb")) {
        std::fputs(text, f);
        std::fclose(f);
    }
}

static std::string source_of(const asset::AssetRegistry& registry, const char* path) {
    for (usize i = 0; i < registry.count(); ++i) {
        if (std::strcmp(registry.get(i).path, path) != 0) continue;
        char out[300];
        registry.source_path(registry.get(i), out, sizeof out);
        return fs::path(out).generic_string();
    }
    return "";
}

// Plugins, mounts and DLC / patch manifests.
static void test_plugins_dlc(const fs::path& root) {
    CHECK(asset::safe_relative_path("Levels/a.csv"));
    CHECK(!asset::safe_relative_path("../a.csv") && !asset::safe_relative_path("a/../../b") && !asset::safe_relative_path("/etc/x"));
    CHECK(!asset::safe_relative_path("C:/x") && !asset::safe_relative_path("a\\b") && !asset::safe_relative_path("") && !asset::safe_relative_path("a//b"));

    asset::Manifest m;
    m.name = "Base";
    m.entries = {{"a.csv", 0xabcdef0123456789ull, 10}, {"Plugins/P/with space.csv", 5, 2}};
    asset::Manifest back;
    CHECK(asset::manifest_from_text(asset::manifest_to_text(m), back));
    CHECK(back.name == "Base" && back.entries.size() == 2 && back.entries[0].hash == 0xabcdef0123456789ull && back.entries[1].path == "Plugins/P/with space.csv");
    CHECK(!asset::manifest_from_text("lsd-manifest 1\nname x\n0000000000000001 3 ../evil.csv\n", back));
    CHECK(!asset::manifest_from_text("not a manifest", back));
    asset::Manifest now = m;
    now.entries[0].hash = 1;                 // changed
    now.entries.push_back({"new.csv", 9, 1});  // added
    const std::vector<std::string> changed = asset::manifest_changed(m, now);
    CHECK(changed.size() == 2 && changed[0] == "a.csv" && changed[1] == "new.csv");
    CHECK(asset::manifest_matches(m, "a.csv", 0xabcdef0123456789ull) && !asset::manifest_matches(m, "a.csv", 1));

    const fs::path dir = root / "plugins_dlc";
    fs::remove_all(dir);
    write_text(dir / "Content/a.csv", "a,1\n");
    write_text(dir / "Content/keep.csv", "k,1\n");
    const std::string plugins = (dir / "Plugins").generic_string();
    std::string why;
    CHECK(asset::create_plugin(plugins.c_str(), "Alpha", why) && asset::create_plugin(plugins.c_str(), "Beta", why));
    CHECK(!asset::create_plugin(plugins.c_str(), "Alpha", why) && !asset::create_plugin(plugins.c_str(), "..", why));
    write_text(dir / "Plugins/Alpha/Content/Sub/b.csv", "b,1\n");
    write_text(dir / "Plugins/Beta/Content/c.csv", "c,1\n");
    write_text(dir / "Plugins/Alpha/Source/Alpha.cpp", "int x;\n");
    CHECK(asset::set_plugin_enabled(plugins.c_str(), "Beta", false));
    std::vector<asset::PluginInfo> found = asset::discover_plugins(plugins.c_str());
    CHECK(found.size() == 2 && std::strcmp(found[0].descriptor.name, "Alpha") == 0 && found[0].descriptor.enabled && !found[1].descriptor.enabled);
    CHECK(asset::plugin_mounts(found).size() == 1 && asset::plugin_source_files(found).size() == 1);

    write_text(dir / "DLC/Patch1/Content/a.csv", "a,patch1\n");
    write_text(dir / "DLC/Patch2/Content/a.csv", "a,patch2\n");
    write_text(dir / "DLC/Patch2/Content/dlc_only.csv", "d,1\n");
    HeapAllocator heap("plugins");
    asset::AssetRegistry registry(heap, (dir / "Content").generic_string().c_str());
    std::vector<asset::AssetMount> mounts = asset::plugin_mounts(found);
    for (const asset::AssetMount& d : asset::dlc_mounts((dir / "DLC").generic_string().c_str())) mounts.push_back(d);
    CHECK(mounts.size() == 3);
    registry.set_mounts(mounts);
    registry.scan();
    CHECK(source_of(registry, "Plugins/Alpha/Sub/b.csv").find("Plugins/Alpha/Content/Sub/b.csv") != std::string::npos);
    CHECK(source_of(registry, "Plugins/Beta/c.csv").empty());                                      // disabled: not mounted
    CHECK(source_of(registry, "a.csv").find("DLC/Patch2/Content/a.csv") != std::string::npos);      // the later package wins
    CHECK(source_of(registry, "keep.csv").find("Content/keep.csv") != std::string::npos);
    CHECK(source_of(registry, "dlc_only.csv").find("DLC/Patch2") != std::string::npos);
    asset::AssetMeta meta;
    for (usize i = 0; i < registry.count(); ++i) {
        if (std::strcmp(registry.get(i).path, "Plugins/Alpha/Sub/b.csv") == 0) {
            CHECK(registry.load_meta(registry.get(i), meta) && meta.guid[0] != '\0');  // the .meta sits in the plugin
            CHECK(std::strcmp(registry.get(i).folder, "Plugins/Alpha/Sub") == 0);
        }
    }
    // Removing the DLC and re-enabling Beta returns the base file and adds the plugin's.
    asset::set_plugin_enabled(plugins.c_str(), "Beta", true);
    registry.set_mounts(asset::plugin_mounts(asset::discover_plugins(plugins.c_str())));
    registry.scan();
    CHECK(source_of(registry, "a.csv").find("Content/a.csv") != std::string::npos && source_of(registry, "a.csv").find("DLC") == std::string::npos);
    CHECK(!source_of(registry, "Plugins/Beta/c.csv").empty() && source_of(registry, "dlc_only.csv").empty());

    // A manifest of a folder, and a patch against it.
    asset::Manifest base = asset::manifest_of_folder((dir / "Content").generic_string().c_str());
    CHECK(base.entries.size() == 2);  // .meta files are not listed
    write_text(dir / "Content/a.csv", "a,2\n");
    write_text(dir / "Content/more.csv", "m,1\n");
    const std::vector<std::string> patch = asset::manifest_changed(base, asset::manifest_of_folder((dir / "Content").generic_string().c_str()));
    CHECK(patch.size() == 2 && patch[0] == "a.csv" && patch[1] == "more.csv");
}

// Thousands of assets: lookups by GUID and by exact name stay right after removals, searches have no cap,
// and an exact name wins over names that only contain it.
static void test_registry_scale(const fs::path& root) {
    HeapAllocator heap("registry");
    const fs::path content = root / "ScaleContent";
    fs::remove_all(content);
    constexpr int kFiles = 3000;
    for (int i = 0; i < kFiles; ++i) {
        const fs::path dir = content / ("F" + std::to_string(i % 30));
        fs::create_directories(dir);
        std::ofstream(dir / ("S_" + std::to_string(i) + ".lua")) << "-- " << i << "\n";
    }
    for (const char* name : {"Rock_Big", "Rock", "Rockery"}) std::ofstream(content / (std::string(name) + ".lua")) << "--\n";
    {
        asset::AssetRegistry registry(heap, content.generic_string().c_str());
        CHECK(registry.scan());
        CHECK(registry.count() == kFiles + 3);
        CHECK(registry.folders().size() == 30 && registry.folders()[0] == "F0");  // the folder walk lists them, A-Z
        const u32 generation = registry.generation();
        CHECK(!registry.scan() && registry.generation() == generation);  // nothing changed on disk: not applied again
        registry.add_folder("F0/New");
        CHECK(registry.folders().size() == 31 && registry.generation() != generation);
        std::vector<usize> all;
        registry.search("S_", asset::AssetType::Script, nullptr, all);
        CHECK(all.size() == kFiles);  // the capped search stopped at its buffer
        const asset::AssetRecord* rock = registry.find_named("Rock", asset::AssetType::Script);
        CHECK(rock && std::strcmp(rock->name, "Rock") == 0);
        CHECK(!registry.find_named("rock") && !registry.find_named("Rock", asset::AssetType::Mesh));
        CHECK(registry.best_match("rock") == rock);                                       // the whole name, any case
        CHECK(registry.best_match("Rocke") && std::strcmp(registry.best_match("Rocke")->name, "Rockery") == 0);
        CHECK(registry.best_match("S_2999") && std::strcmp(registry.best_match("S_2999")->name, "S_2999") == 0);
        // Remove some (records move around): every GUID still finds its own record.
        for (int i = 0; i < kFiles; i += 7) {
            const fs::path file = content / ("F" + std::to_string(i % 30)) / ("S_" + std::to_string(i) + ".lua");
            fs::remove(file);
            fs::remove(file.string() + ".meta");
        }
        registry.scan();
        bool all_found = true;
        for (usize i = 0; i < registry.count(); ++i) all_found = all_found && registry.find(registry.get(i).guid) == &registry.get(i);
        CHECK(all_found && !registry.find_named("S_7") && registry.find_named("S_8"));
    }
    fs::remove_all(content);
    CHECK(heap.allocated_bytes() == 0);
}

// Unreal-style files: _LOD<n> nodes become the mesh's own LODs (not baked into LOD 0), UCX_ hulls are not drawn,
// neither shows up as a part; the compiled mesh keeps the LODs after LOD 0. Also an FBX LOD group (Maya).
static void test_authored_lods(const fs::path& root) {
    CHECK(asset::lod_from_node_name("SM_Rock_LOD1") == 1 && asset::lod_from_node_name("sm_rock_lod12") == 12 && asset::lod_from_node_name("SM_Rock_LOD0") == 0);
    CHECK(asset::lod_from_node_name("SM_Lodge") == -1 && asset::lod_from_node_name("LOD1") == -1 && asset::lod_from_node_name("SM_Rock") == -1);
    CHECK(asset::is_collision_node_name("UCX_SM_Rock_00") && asset::is_collision_node_name("ubx_Wall") && !asset::is_collision_node_name("UCXRock"));
    HeapAllocator heap("lods");
    fs::create_directories(root);
    const fs::path obj = root / "SM_Box.obj";
    {
        std::ofstream f(obj);
        f << "o SM_Box_LOD0\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n"   // 2 triangles
          << "o SM_Box_LOD1\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 5 6 7\n"                       // 1 triangle
          << "o UCX_SM_Box_00\nv 100 0 0\nv 101 0 0\nv 101 1 0\nf 8 9 10\n"              // far away: not drawn
          << "o SM_Box_LOD7\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 11 12 13\n";                   // deeper than LOD 3: left out
    }
    char error[256];
    render::MeshData mesh;
    asset::MeshImportExtras extras;
    CHECK(asset::import_mesh(obj.string().c_str(), 1.0f, heap, mesh, error, sizeof error, -1, asset::kNormalsSmooth, 30.0f, &extras));
    CHECK(mesh.index_count == 6 && extras.lod_count == 1 && extras.lods[0].index_count == 3);
    CHECK(extras.collision_nodes == 1 && extras.skipped_lods == 1);
    CHECK(asset::compute_bounds(mesh).max[0] < 2.0f);  // the hull at x = 100 is not in the mesh
    {
        Array<asset::MeshPart> parts(heap);
        CHECK(asset::list_mesh_parts(obj.string().c_str(), parts, error, sizeof error) && parts.size() == 1);
    }
    // Compiled: LOD 0 as before, the LODs after it.
    const std::string compiled = (root / "SM_Box.lsdmesh").string();
    CHECK(asset::save_compiled_mesh(compiled.c_str(), mesh, {1, 2, 3}, extras.lods, extras.lod_count));
    render::MeshData lod0;
    CHECK(asset::load_compiled_mesh(compiled.c_str(), heap, lod0) && lod0.index_count == 6);
    render::MeshData lods[asset::kMaxAuthoredLods];
    u32 lod_count = 0;
    CHECK(asset::load_compiled_lods(compiled.c_str(), heap, lods, lod_count) && lod_count == 1 && lods[0].index_count == 3 && lods[0].vertex_count == 3);
    for (u32 l = 0; l < lod_count; ++l) render::free_mesh(heap, lods[l]);
    render::free_mesh(heap, lod0);
    render::free_mesh(heap, mesh);
    for (u32 l = 0; l < extras.lod_count; ++l) render::free_mesh(heap, extras.lods[l]);
    // Only LOD 1.. in the file: the most detailed becomes the mesh.
    {
        std::ofstream f(obj);
        f << "o SM_Box_LOD1\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\no SM_Box_LOD2\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 5 6 7\n";
    }
    CHECK(asset::import_mesh(obj.string().c_str(), 1.0f, heap, mesh, error, sizeof error, -1, asset::kNormalsSmooth, 30.0f, &extras));
    CHECK(mesh.index_count == 6 && extras.lod_count == 1 && extras.lods[0].index_count == 3);
    render::free_mesh(heap, mesh);
    for (u32 l = 0; l < extras.lod_count; ++l) render::free_mesh(heap, extras.lods[l]);
    fs::remove(obj);
    fs::remove(compiled);
    // An FBX LOD group: its first child is LOD 0, the next ones its LODs.
    const std::string group = std::string(LSD_UFBX_DATA_DIR) + "/maya_lod_group_7500_binary.fbx";
    render::MeshData whole;
    CHECK(asset::import_mesh(group.c_str(), 1.0f, heap, whole, error, sizeof error, -1, asset::kNormalsSmooth, 30.0f, &extras));
    std::printf("  LOD group: LOD 0 %u triangles, %u LODs (LOD 1 %u triangles)\n", whole.index_count / 3, extras.lod_count,
                extras.lod_count ? extras.lods[0].index_count / 3 : 0);
    CHECK(extras.lod_count >= 1 && extras.lods[0].index_count < whole.index_count);
    render::free_mesh(heap, whole);
    for (u32 l = 0; l < extras.lod_count; ++l) render::free_mesh(heap, extras.lods[l]);
    CHECK(heap.allocated_bytes() == 0);
}

// Cook Max Texture Size: a 64 x 32 image cooked at 16 becomes 16 x 8; one already small enough is left alone.
static void test_cook_texture_cap(const fs::path& root) {
    fs::create_directories(root);
    const std::string path = (root / "T_Big.png").string();
    std::vector<u8> pixels(64 * 32 * 4, 200);
    CHECK(asset::write_png(path.c_str(), 64, 32, pixels.data()));
    CHECK(asset::downscale_texture_file(path.c_str(), 16));
    u32 w = 0, h = 0;
    CHECK(asset::texture_size(path.c_str(), w, h) && w == 16 && h == 8);
    CHECK(!asset::downscale_texture_file(path.c_str(), 16) && !asset::downscale_texture_file(path.c_str(), 0));
    fs::remove(path);
}

int main() {
    test_cook_texture_cap(fs::path(LSD_TEST_TEMP_DIR) / "asset_tests");
    test_authored_lods(fs::path(LSD_TEST_TEMP_DIR) / "asset_tests");
    test_registry_scale(fs::path(LSD_TEST_TEMP_DIR) / "asset_tests");
    test_plugins_dlc(fs::path(LSD_TEST_TEMP_DIR) / "asset_tests");
    test_ies();
    test_map_names();
    test_material_import(fs::path(LSD_TEST_TEMP_DIR) / "asset_tests");
    test_localization();
    test_anim_curves();
    test_fbx_parts();
    test_normals();
    const fs::path root = fs::path(LSD_TEST_TEMP_DIR) / "asset_tests";
    test_guid_and_array();
    test_blender_fbx(root);
    test_gltf_axes(root);
    test_texture(root);
    if (g_failures == 0) std::printf("All asset tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
