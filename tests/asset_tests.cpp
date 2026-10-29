// Asset pipeline tests, including the milestone 2 exit test on a real Blender FBX:
// import it into Content/, find it by search, compile it and load the result.
#include "lsd/asset/asset_registry.h"
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

int main() {
    const fs::path root = fs::path(LSD_TEST_TEMP_DIR) / "asset_tests";
    test_guid_and_array();
    test_blender_fbx(root);
    test_gltf_axes(root);
    if (g_failures == 0) std::printf("All asset tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
