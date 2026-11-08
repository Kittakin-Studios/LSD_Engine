#include "material_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/log.h"
#include "lsd/script/material_graph.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

TextureCache::TextureCache(rhi::Device& device, const asset::AssetRegistry& registry)
    : compress_(device.supports_bc()), device_(device), registry_(registry) {
    worker_ = std::thread([this] {  // decodes queued files, one at a time
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                if (quit_) return;
                job = jobs_.front();
                jobs_.erase(jobs_.begin());
            }
            auto* data = new asset::TextureData(default_allocator());
            // Colour maps (BC1 / BC3) and normal maps (BC5) are compressed, once, into DerivedData; masks (ORM) stay RGBA8.
            if (!asset::load_texture_cached(job.path, job.srgb, job.max_size, (job.srgb || job.normal_map) && compress_, "DerivedData/Textures", *data, job.normal_map)) {
                delete data;
                data = nullptr;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            finished_.push_back({job.entry, data});
        }
    });
}

TextureCache::~TextureCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (const Finished& f : finished_) delete f.data;
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].texture.valid()) device_.destroy_texture(entries_[i].texture);
    }
}

bool TextureCache::prepare(Entry& e, char* path, usize capacity, u32& max_size) {
    if (!path_of(registry_, e.guid, path, capacity)) return false;
    e.modified = file_time(path);
    asset::Guid guid;
    asset::parse_guid(e.guid, guid);
    asset::AssetMeta meta;
    if (const asset::AssetRecord* r = registry_.find(guid)) registry_.load_meta(*r, meta);
    max_size = meta.max_size;
    if (size_cap_ && (!max_size || max_size > size_cap_)) max_size = size_cap_;  // little video memory: smaller mips
    return true;
}

void TextureCache::upload(Entry& e, const asset::TextureData& data) {
    const void* mips[asset::kMaxTextureMips];
    for (u32 m = 0; m < data.mip_count; ++m) mips[m] = data.mip(m);
    rhi::TextureFormat format = e.srgb ? rhi::TextureFormat::RGBA8_sRGB : rhi::TextureFormat::RGBA8;
    if (data.encoding == asset::TextureEncoding::BC1) format = e.srgb ? rhi::TextureFormat::BC1_sRGB : rhi::TextureFormat::BC1;
    if (data.encoding == asset::TextureEncoding::BC3) format = e.srgb ? rhi::TextureFormat::BC3_sRGB : rhi::TextureFormat::BC3;
    if (data.encoding == asset::TextureEncoding::BC5) format = rhi::TextureFormat::BC5;
    const rhi::TextureHandle texture = device_.create_texture(data.width, data.height, data.mip_count, format, mips);
    if (!texture.valid()) return;  // keeps the old one, if any
    if (e.texture.valid()) device_.destroy_texture(e.texture);
    e.texture = texture;
    e.failed = false;
}

bool TextureCache::load(Entry& e) {
    char path[300];
    u32 max_size = 0;
    if (!prepare(e, path, sizeof path, max_size)) {
        e.failed = true;
        return false;
    }
    asset::TextureData data(default_allocator());
    if (!asset::load_texture_cached(path, e.srgb, max_size, (e.srgb || e.normal_map) && compress_, "DerivedData/Textures", data, e.normal_map)) {
        e.failed = !e.texture.valid();
        return false;
    }
    upload(e, data);
    return e.texture.valid();
}

void TextureCache::queue(u32 index) {
    Entry& e = entries_[index];
    Job job{index, "", e.srgb, 0, e.normal_map};
    if (!prepare(e, job.path, sizeof job.path, job.max_size)) {
        e.failed = true;
        return;
    }
    e.pending = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(job);
    }
    wake_.notify_one();
}

void TextureCache::update(u32 budget) {
    std::vector<Finished> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const usize n = finished_.size() < budget ? finished_.size() : budget;
        ready.assign(finished_.begin(), finished_.begin() + static_cast<std::ptrdiff_t>(n));
        finished_.erase(finished_.begin(), finished_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    for (const Finished& f : ready) {
        Entry& e = entries_[f.entry];
        e.pending = false;
        if (f.data) upload(e, *f.data);
        else e.failed = !e.texture.valid();
        delete f.data;
    }
}

u32 TextureCache::streaming() const {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) n += entries_[i].pending ? 1u : 0u;
    return n;
}

rhi::TextureHandle TextureCache::get(const char* guid, bool srgb, bool normal_map) {
    if (!guid || !guid[0]) return {};
    const u32 use = srgb ? 1u : normal_map ? 2u : 0u;
    const i64 slot = index_.find(guid, use, [&](u32 i) {
        return entries_[i].srgb == srgb && entries_[i].normal_map == normal_map && std::strcmp(entries_[i].guid, guid) == 0;
    });
    if (slot >= 0) return entries_[slot].texture;
    if (count_ == sizeof entries_ / sizeof entries_[0]) return {};
    index_.add(guid, use, count_);
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    e.srgb = srgb;
    e.normal_map = normal_map && !srgb;
    if (stream) queue(count_ - 1);  // drawn without it for a few frames
    else load(e);
    return e.texture;
}

void TextureCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        char path[300];
        if (!e.pending && path_of(registry_, e.guid, path, sizeof path) && file_time(path) != e.modified) {
            LSD_LOG_INFO("editor", "Reloading texture %s", path);
            if (stream) queue(i);  // the old one stays until the new one is up
            else load(e);
        }
    }
}

MaterialCache::Entry* MaterialCache::find(const char* guid) {
    const i64 slot = index_.find(guid, 0, [&](u32 i) { return std::strcmp(entries_[i].guid, guid) == 0; });
    if (slot >= 0) return &entries_[slot];
    if (count_ == sizeof entries_ / sizeof entries_[0]) return nullptr;
    index_.add(guid, 0, count_);
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    load(e);
    return &e;
}

bool find_texture_guid(const asset::AssetRegistry& registry, const char* name, char out[33]) {
    out[0] = '\0';
    usize hits[16];
    const usize count = registry.search(name, asset::AssetType::Texture, nullptr, hits, 16);
    for (usize k = 0; k < count; ++k) {  // the exact name (search also matches parts of names)
        const asset::AssetRecord& r = registry.get(hits[k]);
        if (std::strcmp(r.name, name) == 0 || k + 1 == count) asset::to_string(r.guid, out);
        if (std::strcmp(r.name, name) == 0) break;
    }
    return out[0] != '\0';
}

void MaterialCache::resolve_textures(Entry& e) {
    for (u32 i = 0; i < e.texture_count; ++i) {
        if (!find_texture_guid(registry_, e.textures[i], e.texture_guids[i])) LSD_LOG_WARN("material", "Material graph texture '%s' not found", e.textures[i]);
    }
}

void MaterialCache::load_graph(Entry& e, const char* path) {
    e.texture_count = 0;
    e.blend_mode = 0;
    script::Graph graph(default_allocator());
    script::MaterialCode code(default_allocator());
    char error[256] = "";
    const bool shader_asset = std::filesystem::path(path).extension() == script::kShaderExtension;
    bool ok = false;
    if (shader_asset) {  // a material written as code
        std::ifstream f(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ok = !text.empty() && script::parse_shader_source(text.c_str(), code, error, sizeof error);
        e.blend_mode = code.blend_mode;
    } else {
        ok = script::load_graph_file(graph, path) && script::compile_material_graph(graph, code, error, sizeof error);
    }
    if (!ok) {
        LSD_LOG_ERROR("material", "%s: %s", path, error[0] ? error : shader_asset ? "could not read the shader" : "could not read the graph");
        e.failed = true;
        return;
    }
    e.shader = device_.create_material_shader(code.code.data());
    if (!e.shader.valid()) {
        LSD_LOG_ERROR("material", "%s: the shader did not compile (see the messages above)", path);
        e.failed = true;
        return;
    }
    e.param_count = code.param_count;
    for (u32 i = 0; i < code.param_count; ++i) {
        std::snprintf(e.param_names[i], sizeof e.param_names[i], "%s", code.params[i].name);
        std::memcpy(e.params[i], code.params[i].value, sizeof e.params[i]);
    }
    e.texture_count = code.texture_count;
    for (u32 i = 0; i < code.texture_count; ++i) {
        std::snprintf(e.textures[i], sizeof e.textures[i], "%s", code.textures[i].name);
        e.texture_srgb[i] = code.textures[i].srgb;
    }
    resolve_textures(e);
    LSD_LOG_INFO("material", shader_asset ? "Compiled shader %s" : "Compiled material graph %s", path);
}

void MaterialCache::load(Entry& e) {
    e.material = render::Material{};
    e.parent[0] = '\0';
    e.parent_modified = 0;
    if (e.shader.valid()) device_.destroy_material_shader(e.shader);
    e.shader = {};
    e.texture_count = 0;
    e.param_count = 0;
    e.graph_instance = false;
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
        asset::Guid parent_guid;
        const asset::AssetRecord* parent_record = asset::load_data_asset(path, instance) && asset::parse_guid(instance.parent, parent_guid) ? registry_.find(parent_guid) : nullptr;
        if (parent_record && parent_record->type == asset::AssetType::MaterialGraph && path_of(registry_, instance.parent, parent_path, sizeof parent_path)) {
            // An instance of a graph: the graph's shader with these parameter values.
            Entry* graph = find(instance.parent);
            if (graph && graph->modified != file_time(parent_path)) load(*graph);  // the graph changed: its defaults first
            e.failed = !graph || graph->failed;
            if (e.failed) return;
            std::snprintf(e.parent, sizeof e.parent, "%s", instance.parent);
            e.parent_modified = file_time(parent_path);
            e.graph_instance = true;
            e.param_count = graph->param_count;
            std::memcpy(e.params, graph->params, sizeof e.params);
            std::memcpy(e.param_names, graph->param_names, sizeof e.param_names);
            for (const render::MaterialParameter& p : instance.parameters) {
                for (u32 i = 0; i < e.param_count; ++i) {
                    if (std::strcmp(e.param_names[i], p.name) != 0) continue;
                    e.params[i][0] = p.value.x;
                    e.params[i][1] = p.value.y;
                    e.params[i][2] = p.value.z;
                }
            }
            return;
        }
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
    if (e->shader.valid() || e->graph_instance) {
        const Entry* graph = e->graph_instance ? find(e->parent) : e;  // an instance draws with its graph's shader and textures
        if (!graph || graph->failed || !graph->shader.valid()) return;
        draw.material_shader = graph->shader;
        draw.material_params = &e->params[0][0];
        draw.material_param_count = e->param_count;
        rhi::TextureHandle* slots[4] = {&draw.base_color_texture, &draw.normal_texture, &draw.orm_texture, &draw.emissive_texture};
        for (u32 i = 0; i < graph->texture_count; ++i) {
            *slots[i] = graph->texture_guids[i][0] ? textures_.get(graph->texture_guids[i], graph->texture_srgb[i]) : rhi::TextureHandle{};
        }
        draw.blend = graph->blend_mode == 2 ? 1u : graph->blend_mode == 3 ? 2u : 0u;
        draw.alpha_cutoff = graph->blend_mode == 1 ? 0.333f : 0.0f;  // shortcut: graphs use Unreal's default clip; add a pin if it needs changing
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
    draw.normal_texture = textures_.get(m.normal_texture, false, true);
    draw.orm_texture = textures_.get(m.orm_texture, false);
    draw.emissive_texture = textures_.get(m.emissive_texture, true);
    // Blend Mode: Opaque, Masked (cut-outs), Translucent, Additive; Opacity scales the alpha.
    if (m.blend_mode != 0) draw.color[3] *= m.opacity;
    draw.blend = m.blend_mode == 2 ? 1u : m.blend_mode == 3 ? 2u : 0u;
    draw.alpha_cutoff = m.blend_mode == 1 ? (m.opacity_mask_clip > 0.0f ? m.opacity_mask_clip : 0.001f) : 0.0f;
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
