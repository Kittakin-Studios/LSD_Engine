#include "thumbnails.h"

#include "lsd/asset/mesh_import.h"
#include "lsd/foundation/math.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {
constexpr u32 kThumbnailSize = 128;
}

ThumbnailCache::ThumbnailCache(rhi::Device& device, Allocator& allocator, const asset::AssetRegistry& registry, const char* data_dir)
    : device_(device), allocator_(allocator), registry_(registry), entries_(allocator), temporaries_(allocator) {
    std::snprintf(data_dir_, sizeof data_dir_, "%s", data_dir);
}

ThumbnailCache::~ThumbnailCache() {
    release_temporaries();
    for (Entry& e : entries_) device_.destroy_texture(e.texture);
}

u64 ThumbnailCache::get(asset::Guid guid) {
    const asset::AssetRecord* record = registry_.find(guid);
    for (Entry& e : entries_) {
        if (e.guid != guid) continue;
        if (record && (e.ready || e.failed) && record->source_time != e.source_time) e.ready = e.failed = false;  // file changed: redo
        return e.ready ? device_.debug_ui_texture(e.texture) : 0;
    }
    Entry e;
    e.guid = guid;
    entries_.push_back(e);
    return 0;
}

void ThumbnailCache::release_temporaries() {
    for (render::GpuMesh& m : temporaries_) render::destroy_mesh(device_, m);
    temporaries_.clear();
}

void ThumbnailCache::render(const f32 background[4], u32 budget) {
    bool rendered = false;
    for (Entry& e : entries_) {
        if (budget == 0) break;
        if (e.ready || e.failed) continue;
        if (const asset::AssetRecord* r = registry_.find(e.guid)) e.source_time = r->source_time;  // also on failure: retry only after a change
        e.failed = !render_one(e, background);
        e.ready = !e.failed;
        rendered = true;
        --budget;
    }
    if (rendered) device_.set_render_target({}, nullptr);
}

bool ThumbnailCache::render_one(Entry& entry, const f32 background[4]) {
    const asset::AssetRecord* record = registry_.find(entry.guid);
    if (!record || record->type != asset::AssetType::Mesh) return false;
    char compiled[300];
    if (!asset::compile_mesh_asset(registry_, *record, data_dir_, allocator_, compiled, sizeof compiled)) return false;
    render::MeshData data;
    asset::MeshBounds bounds;
    if (!asset::load_compiled_mesh(compiled, allocator_, data, &bounds)) return false;
    const render::GpuMesh mesh = render::upload_mesh(device_, data);
    render::free_mesh(allocator_, data);
    temporaries_.push_back(mesh);  // the GPU reads it this frame; freed next frame
    if (!entry.texture.valid()) entry.texture = device_.create_render_target(kThumbnailSize, kThumbnailSize);
    if (!entry.texture.valid()) return false;

    // Frame the bounding sphere from the editor's default viewing angle.
    const Vec3 lo{bounds.min[0], bounds.min[1], bounds.min[2]};
    const Vec3 hi{bounds.max[0], bounds.max[1], bounds.max[2]};
    const Vec3 center = (lo + hi) * 0.5f;
    const f32 radius = length(hi - lo) * 0.5f > 1e-4f ? length(hi - lo) * 0.5f : 1.0f;
    constexpr f32 kFov = 0.6f, kYaw = -2.4f, kPitch = 0.45f;
    const f32 distance = radius / std::sin(kFov * 0.5f) * 1.05f;
    const Vec3 dir{std::cos(kPitch) * std::cos(kYaw), std::cos(kPitch) * std::sin(kYaw), std::sin(kPitch)};
    const Mat4 view_proj = perspective(kFov, 1.0f, distance * 0.01f, distance + radius * 2.0f) * look_at(center + dir * distance, center, Vec3{0, 0, 1});

    device_.set_render_target(entry.texture, background);
    rhi::MeshDraw draw{};
    draw.vertices = mesh.vertices;
    draw.indices = mesh.indices;
    draw.index_count = mesh.index_count;
    std::memcpy(draw.mvp, view_proj.m, sizeof draw.mvp);
    const f32 identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(draw.normal_matrix, identity, sizeof identity);
    std::memcpy(draw.world, identity, sizeof identity);
    const Vec3 eye = center + dir * distance;
    draw.camera_position[0] = eye.x;
    draw.camera_position[1] = eye.y;
    draw.camera_position[2] = eye.z;
    const f32 grey[4] = {0.75f, 0.75f, 0.75f, 1.0f};
    std::memcpy(draw.color, grey, sizeof grey);
    device_.draw_mesh(draw);
    return true;
}

} // namespace lsd::editor
