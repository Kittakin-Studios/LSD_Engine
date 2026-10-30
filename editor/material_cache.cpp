#include "material_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/script/material_graph.h"

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
    load(e);
    return &e;
}

void MaterialCache::resolve_textures(Entry& e) {
    for (u32 i = 0; i < e.texture_count; ++i) {
        e.texture_guids[i][0] = '\0';
        usize hits[16];
        const usize count = registry_.search(e.textures[i], asset::AssetType::Texture, nullptr, hits, 16);
        for (usize k = 0; k < count; ++k) {  // the exact name (search also matches parts of names)
            const asset::AssetRecord& r = registry_.get(hits[k]);
            if (std::strcmp(r.name, e.textures[i]) == 0 || k + 1 == count) asset::to_string(r.guid, e.texture_guids[i]);
            if (std::strcmp(r.name, e.textures[i]) == 0) break;
        }
        if (!e.texture_guids[i][0]) LSD_LOG_WARN("material", "Material graph texture '%s' not found", e.textures[i]);
    }
}

void MaterialCache::load_graph(Entry& e, const char* path) {
    e.texture_count = 0;
    script::Graph graph(default_allocator());
    script::MaterialCode code(default_allocator());
    char error[256] = "";
    if (!script::load_graph_file(graph, path) || !script::compile_material_graph(graph, code, error, sizeof error)) {
        LSD_LOG_ERROR("material", "%s: %s", path, error[0] ? error : "could not read the graph");
        e.failed = true;
        return;
    }
    e.shader = device_.create_material_shader(code.code.data());
    if (!e.shader.valid()) {
        LSD_LOG_ERROR("material", "%s: the shader did not compile (see the messages above)", path);
        e.failed = true;
        return;
    }
    e.texture_count = code.texture_count;
    for (u32 i = 0; i < code.texture_count; ++i) {
        std::snprintf(e.textures[i], sizeof e.textures[i], "%s", code.textures[i].name);
        e.texture_srgb[i] = code.textures[i].srgb;
    }
    resolve_textures(e);
    LSD_LOG_INFO("material", "Compiled material graph %s", path);
}

void MaterialCache::load(Entry& e) {
    e.material = render::Material{};
    e.parent[0] = '\0';
    e.parent_modified = 0;
    if (e.shader.valid()) device_.destroy_material_shader(e.shader);
    e.shader = {};
    e.texture_count = 0;
    char path[300], type[32] = "";
    asset::Guid guid;
    const asset::AssetRecord* record = asset::parse_guid(e.guid, guid) ? registry_.find(guid) : nullptr;
    if (record && record->type == asset::AssetType::MaterialGraph && path_of(registry_, e.guid, path, sizeof path)) {
        e.modified = file_time(path);
        e.failed = false;
        load_graph(e, path);
        return;
    }
    e.failed = !path_of(registry_, e.guid, path, sizeof path) || !asset::read_data_asset_type(path, type, sizeof type);
    e.modified = e.failed ? 0 : file_time(path);
    if (e.failed) return;
    if (std::strcmp(type, "MaterialInstance") == 0) {
        render::MaterialInstance instance;
        char parent_path[300];
        render::Material parent;
        e.failed = !asset::load_data_asset(path, instance) || !path_of(registry_, instance.parent, parent_path, sizeof parent_path) ||
                   !asset::load_data_asset(parent_path, parent);
        if (e.failed) return;
        std::snprintf(e.parent, sizeof e.parent, "%s", instance.parent);
        e.parent_modified = file_time(parent_path);
        e.material = render::resolve(parent, instance);
        return;
    }
    e.failed = !asset::load_data_asset(path, e.material);
}

void MaterialCache::apply(const char* guid, rhi::MeshDraw& draw) {
    const Entry* e = guid && guid[0] ? find(guid) : nullptr;
    if (!e || e->failed) return;
    if (e->shader.valid()) {
        draw.material_shader = e->shader;
        rhi::TextureHandle* slots[3] = {&draw.base_color_texture, &draw.normal_texture, &draw.orm_texture};
        for (u32 i = 0; i < e->texture_count; ++i) *slots[i] = e->texture_guids[i][0] ? textures_.get(e->texture_guids[i], e->texture_srgb[i]) : rhi::TextureHandle{};
        return;
    }
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
        char path[300], parent_path[300];
        if (!path_of(registry_, e.guid, path, sizeof path)) continue;
        const bool parent_changed = e.parent[0] && path_of(registry_, e.parent, parent_path, sizeof parent_path) && file_time(parent_path) != e.parent_modified;
        if (file_time(path) != e.modified || parent_changed) load(e);
        else if (e.shader.valid()) resolve_textures(e);  // a texture imported after the graph was compiled
    }
}

} // namespace lsd::editor
