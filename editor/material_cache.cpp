#include "material_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lsd::editor {

namespace {

i64 file_time(const char* path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

// The asset's file path, or false if the GUID names no asset.
bool path_of(const asset::AssetRegistry& registry, const char* guid_text, char* out, usize capacity) {
    asset::Guid guid;
    const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (!r) return false;
    registry.source_path(*r, out, capacity);
    return true;
}

} // namespace

TextureCache::~TextureCache() {
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].texture.valid()) device_.destroy_texture(entries_[i].texture);
    }
}

bool TextureCache::load(Entry& e) {
    if (e.texture.valid()) device_.destroy_texture(e.texture);
    e.texture = {};
    char path[300];
    e.failed = true;
    if (!path_of(registry_, e.guid, path, sizeof path)) return false;
    e.modified = file_time(path);
    asset::Guid guid;
    asset::parse_guid(e.guid, guid);
    asset::AssetMeta meta;
    if (const asset::AssetRecord* r = registry_.find(guid)) registry_.load_meta(*r, meta);
    asset::TextureData data(default_allocator());
    if (!asset::load_texture(path, e.srgb, meta.max_size, data)) return false;
    const void* mips[asset::kMaxTextureMips];
    for (u32 m = 0; m < data.mip_count; ++m) mips[m] = data.mip(m);
    e.texture = device_.create_texture(data.width, data.height, data.mip_count, e.srgb ? rhi::TextureFormat::RGBA8_sRGB : rhi::TextureFormat::RGBA8, mips);
    e.failed = !e.texture.valid();
    return !e.failed;
}

rhi::TextureHandle TextureCache::get(const char* guid, bool srgb) {
    if (!guid || !guid[0]) return {};
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].srgb == srgb && std::strcmp(entries_[i].guid, guid) == 0) return entries_[i].texture;
    }
    if (count_ == sizeof entries_ / sizeof entries_[0]) return {};
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    e.srgb = srgb;
    load(e);
    return e.texture;
}

void TextureCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        char path[300];
        if (path_of(registry_, e.guid, path, sizeof path) && file_time(path) != e.modified) {
            if (load(e)) LSD_LOG_INFO("editor", "Reloaded texture %s", path);
        }
    }
}

MaterialCache::Entry* MaterialCache::find(const char* guid) {
    for (u32 i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].guid, guid) == 0) return &entries_[i];
    }
    if (count_ == sizeof entries_ / sizeof entries_[0]) return nullptr;
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    char path[300];
    e.failed = !path_of(registry_, guid, path, sizeof path) || !asset::load_data_asset(path, e.material);
    e.modified = e.failed ? 0 : file_time(path);
    return &e;
}

void MaterialCache::apply(const char* guid, rhi::MeshDraw& draw) {
    const Entry* e = guid && guid[0] ? find(guid) : nullptr;
    if (!e || e->failed) return;
    const render::Material& m = e->material;
    draw.color[0] *= m.base_color.r;
    draw.color[1] *= m.base_color.g;
    draw.color[2] *= m.base_color.b;
    draw.color[3] *= m.base_color.a;
    draw.metallic = m.metallic;
    draw.roughness = m.roughness;
    draw.emissive[0] = m.emissive.r * m.emissive_strength;
    draw.emissive[1] = m.emissive.g * m.emissive_strength;
    draw.emissive[2] = m.emissive.b * m.emissive_strength;
    draw.base_color_texture = textures_.get(m.base_color_texture, true);
    draw.normal_texture = textures_.get(m.normal_texture, false);
    draw.orm_texture = textures_.get(m.orm_texture, false);
}

void MaterialCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        char path[300];
        if (!path_of(registry_, e.guid, path, sizeof path) || file_time(path) == e.modified) continue;
        e.material = render::Material{};
        e.failed = !asset::load_data_asset(path, e.material);
        e.modified = file_time(path);
    }
}

} // namespace lsd::editor
