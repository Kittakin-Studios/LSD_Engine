#include "thumbnails.h"

#include "material_cache.h"

#include "lsd/asset/mesh_import.h"
#include "lsd/audio/audio.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

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
    if (record && load_cached(e, record->source_time)) {
        entries_.push_back(e);
        return device_.debug_ui_texture(e.texture);
    }
    entries_.push_back(e);
    return 0;
}

std::string ThumbnailCache::cache_path(asset::Guid guid, i64 source_time) const {
    char name[33];
    asset::to_string(guid, name);
    return std::string(data_dir_) + "/Thumbnails/" + name + "-" + std::to_string(source_time) + ".png";
}

bool ThumbnailCache::load_cached(Entry& e, i64 source_time) {
    const std::string path = cache_path(e.guid, source_time);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return false;
    asset::TextureData data(allocator_);
    if (!asset::load_texture(path.c_str(), false, 0, data) || data.width != kThumbnailSize || data.height != kThumbnailSize) return false;
    // Saved top row first; render targets are shown flipped on some backends, so match them.
    if (device_.render_targets_flipped()) {
        const usize row = static_cast<usize>(data.width) * 4;
        std::vector<u8> swap(row);
        for (u32 y = 0; y < data.height / 2; ++y) {
            u8* a = data.pixels.data() + y * row;
            u8* b = data.pixels.data() + (data.height - 1 - y) * row;
            std::memcpy(swap.data(), a, row);
            std::memcpy(a, b, row);
            std::memcpy(b, swap.data(), row);
        }
    }
    const void* mips[1] = {data.pixels.data()};
    const rhi::TextureHandle texture = device_.create_texture(data.width, data.height, 1, rhi::TextureFormat::RGBA8, mips);
    if (!texture.valid()) return false;
    if (e.texture.valid()) device_.destroy_texture(e.texture);
    e.texture = texture;
    e.from_disk = true;
    e.ready = true;
    e.source_time = source_time;
    return true;
}

void ThumbnailCache::release_temporaries() {
    for (render::GpuMesh& m : temporaries_) render::destroy_mesh(device_, m);
    temporaries_.clear();
    // Last frame's renders to disk (the GPU has finished them by now).
    for (Entry& e : entries_) {
        if (!e.save) continue;
        e.save = false;
        std::vector<u8> pixels(static_cast<usize>(kThumbnailSize) * kThumbnailSize * 4);
        if (!device_.read_render_target(e.texture, pixels.data())) continue;
        std::error_code error;
        const std::filesystem::path folder = std::filesystem::path(data_dir_) / "Thumbnails";
        std::filesystem::create_directories(folder, error);
        char name[33];
        asset::to_string(e.guid, name);
        for (const auto& old : std::filesystem::directory_iterator(folder, error)) {  // the thumbnail of an older version
            if (old.path().filename().string().rfind(std::string(name) + "-", 0) == 0) std::filesystem::remove(old.path(), error);
        }
        asset::write_png(cache_path(e.guid, e.source_time).c_str(), kThumbnailSize, kThumbnailSize, pixels.data());
    }
}

void ThumbnailCache::render(const f32 background[4], u32 budget) {
    bool rendered = false;
    for (Entry& e : entries_) {
        if (budget == 0) break;
        if (e.ready || e.failed) continue;
        if (const asset::AssetRecord* r = registry_.find(e.guid)) e.source_time = r->source_time;  // also on failure: retry only after a change
        if (load_cached(e, e.source_time)) continue;  // another start saved this version already
        const asset::AssetRecord* r = registry_.find(e.guid);
        if (r && r->type == asset::AssetType::Sound) {  // drawn on the CPU: no render target, nothing to read back
            e.failed = !draw_waveform(e, *r, background);
            e.ready = !e.failed;
            --budget;
            continue;
        }
        e.failed = !render_one(e, background);
        e.ready = !e.failed;
        e.save = e.ready;
        rendered = true;
        --budget;
    }
    if (rendered) device_.set_render_target({}, nullptr);
}

// Sounds: the waveform (peaks per column, mirrored about the middle) in the Content Browser's orange.
bool ThumbnailCache::draw_waveform(Entry& entry, const asset::AssetRecord& record, const f32 background[4]) {
    char path[300];
    registry_.source_path(record, path, sizeof path);
    f32 peaks[kThumbnailSize];
    if (!audio::waveform(path, kThumbnailSize, peaks)) return false;
    std::vector<u8> pixels(static_cast<usize>(kThumbnailSize) * kThumbnailSize * 4);
    const u8 back[4] = {static_cast<u8>(background[0] * 255.0f), static_cast<u8>(background[1] * 255.0f), static_cast<u8>(background[2] * 255.0f), 255};
    const u8 wave[4] = {0xEE, 0xA7, 0x0E, 255};
    constexpr f32 kMiddle = kThumbnailSize * 0.5f;
    for (u32 x = 0; x < kThumbnailSize; ++x) {
        const f32 half = std::fmax(peaks[x] * (kMiddle - 4.0f), 0.5f);  // silence still shows a line
        for (u32 y = 0; y < kThumbnailSize; ++y) {
            const bool inside = std::fabs(static_cast<f32>(y) + 0.5f - kMiddle) <= half;
            std::memcpy(&pixels[(static_cast<usize>(y) * kThumbnailSize + x) * 4], inside ? wave : back, 4);
        }
    }
    const std::filesystem::path folder = std::filesystem::path(data_dir_) / "Thumbnails";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return asset::write_png(cache_path(entry.guid, entry.source_time).c_str(), kThumbnailSize, kThumbnailSize, pixels.data()) &&
           load_cached(entry, entry.source_time);
}

bool ThumbnailCache::has_thumbnail(const asset::AssetRecord& r) {
    const bool material = r.type == asset::AssetType::MaterialGraph ||
                          (r.type == asset::AssetType::Data && (std::strcmp(r.data_type, "Material") == 0 || std::strcmp(r.data_type, "MaterialInstance") == 0));
    return r.type == asset::AssetType::Mesh || r.type == asset::AssetType::Sound || material;
}

bool ThumbnailCache::render_one(Entry& entry, const f32 background[4]) {
    const asset::AssetRecord* record = registry_.find(entry.guid);
    if (!record || !has_thumbnail(*record) || record->type == asset::AssetType::Sound) return false;
    const bool material = record->type != asset::AssetType::Mesh;
    if (material && !materials_) return false;
    render::MeshData data;
    asset::MeshBounds bounds;
    if (material) {  // a sphere wearing it
        data = render::make_sphere(allocator_);
        bounds = asset::compute_bounds(data);
    } else {
        char compiled[300];
        if (!asset::compile_mesh_asset(registry_, *record, data_dir_, allocator_, compiled, sizeof compiled)) return false;
        if (!asset::load_compiled_mesh(compiled, allocator_, data, &bounds)) return false;
    }
    const render::GpuMesh mesh = render::upload_mesh(device_, data);
    render::free_mesh(allocator_, data);
    temporaries_.push_back(mesh);  // the GPU reads it this frame; freed next frame
    if (entry.from_disk) {  // a cached picture of an older version: render into a target now
        device_.destroy_texture(entry.texture);
        entry.texture = {};
        entry.from_disk = false;
    }
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
    const f32 white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::memcpy(draw.color, material ? white : grey, sizeof grey);
    if (material) {
        char guid[33];
        asset::to_string(entry.guid, guid);
        materials_->apply(guid, draw);  // base colour, textures, metallic / roughness, a graph's shader
    }
    device_.draw_mesh(draw);
    return true;
}

} // namespace lsd::editor
