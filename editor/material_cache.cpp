#include "material_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/asset/material_import.h"
#include "lsd/asset/texture.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/log.h"
#include "lsd/render/texture_streaming.h"
#include "lsd/script/material_graph.h"

#include <chrono>
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

// A texture's file and its .meta together: Saving its settings (Max Size, Mip Streaming) reloads it like an edit.
i64 texture_time(const char* path) {
    return file_time(path) + file_time((std::string(path) + ".meta").c_str());
}

// The same times as the registry's last folder walk saw them (no file system call): the once-a-second checks
// of every texture and material in use cost a stat per file each second otherwise. -1: no such asset.
i64 walked_time(const asset::AssetRegistry& registry, const char* guid_text, bool with_meta) {
    asset::Guid guid;
    const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    return r ? r->source_time + (with_meta ? r->meta_time : 0) : -1;
}

} // namespace

TextureCache::TextureCache(rhi::Device& device, const asset::AssetRegistry& registry)
    : compress_(device.supports_bc()), device_(device), registry_(registry) {
    // Decoders: a quarter of the cores (at least 2), each taking the next queued file; the mesh compiler and the frame keep the rest.
    const u32 cores = std::thread::hardware_concurrency();
    const u32 count = cores / 4 > 2 ? cores / 4 : 2;
    for (u32 w = 0; w < count; ++w) workers_.emplace_back([this] {
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
            if (!asset::load_texture_cached(job.path, job.srgb, job.max_size, (job.srgb || job.normal_map) && compress_, "DerivedData/Textures", *data, job.normal_map, true)) {
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
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    for (const Finished& f : finished_) delete f.data;
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].texture.valid()) device_.destroy_texture(entries_[i].texture);
    }
}

bool TextureCache::prepare(Entry& e, char* path, usize capacity, u32& max_size) {
    if (const auto it = files_.find(e.guid); it != files_.end()) {  // packed on the fly: no asset, no .meta
        std::snprintf(path, capacity, "%s", it->second.c_str());
        e.modified = texture_time(path);
        max_size = size_cap_;
        e.streams = true;
        e.cap = max_size;
        return true;
    }
    if (!path_of(registry_, e.guid, path, capacity)) return false;
    e.modified = texture_time(path);
    asset::Guid guid;
    asset::parse_guid(e.guid, guid);
    asset::AssetMeta meta;
    if (const asset::AssetRecord* r = registry_.find(guid)) registry_.load_meta(*r, meta);
    max_size = meta.max_size;
    e.streams = meta.mip_streaming;
    if (size_cap_ && (!max_size || max_size > size_cap_)) max_size = size_cap_;  // little video memory: smaller mips
    e.cap = max_size;
    return true;
}

void TextureCache::upload(Entry& e, const asset::TextureData& data) {
    const void* mips[asset::kMaxTextureMips];
    for (u32 m = 0; m < data.mip_count; ++m) mips[m] = data.mip(m);
    rhi::TextureFormat format = e.srgb ? rhi::TextureFormat::RGBA8_sRGB : rhi::TextureFormat::RGBA8;
    if (data.encoding == asset::TextureEncoding::BC1) format = e.srgb ? rhi::TextureFormat::BC1_sRGB : rhi::TextureFormat::BC1;
    if (data.encoding == asset::TextureEncoding::BC3) format = e.srgb ? rhi::TextureFormat::BC3_sRGB : rhi::TextureFormat::BC3;
    if (data.encoding == asset::TextureEncoding::BC5) format = rhi::TextureFormat::BC5;
    if (data.encoding == asset::TextureEncoding::RGBA16F) format = rhi::TextureFormat::RGBA16F;  // HDR images: linear, above 1 kept
    const rhi::TextureHandle texture = device_.create_texture(data.width, data.height, data.mip_count, format, mips);
    if (!texture.valid()) return;  // keeps the old one, if any
    if (e.texture.valid()) device_.destroy_texture(e.texture);
    e.texture = texture;
    e.failed = false;
    e.side = data.width > data.height ? data.width : data.height;
    e.bytes = data.pixels.size();
    e.loaded_texels = static_cast<u64>(data.width) * data.height;
}

bool TextureCache::load(Entry& e) {
    char path[300];
    u32 max_size = 0;
    if (!prepare(e, path, sizeof path, max_size)) {
        e.failed = true;
        return false;
    }
    asset::TextureData data(default_allocator());
    if (!asset::load_texture_cached(path, e.srgb, max_size, (e.srgb || e.normal_map) && compress_, "DerivedData/Textures", data, e.normal_map, true)) {
        e.failed = !e.texture.valid();
        return false;
    }
    e.requested = 0;
    e.complete = true;
    upload(e, data);
    return e.texture.valid();
}

u32 TextureCache::stream_size(u32 want) {
    const u32 size = render::stream_size(want);
    return size == render::kStreamWhole ? 0 : size;
}

u32 TextureCache::loaded_size(rhi::TextureHandle texture) const {
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].texture.value == texture.value && texture.valid()) return entries_[i].requested ? entries_[i].requested : 0xFFFFFFFFu;
    }
    return 0;
}

void TextureCache::stats(u32& textures, u32& reduced, u64& texels) const {
    textures = reduced = 0;
    texels = 0;
    for (u32 i = 0; i < count_; ++i) {
        const Entry& e = entries_[i];
        if (!e.texture.valid()) continue;
        ++textures;
        reduced += e.complete ? 0u : 1u;
        texels += e.loaded_texels;
    }
}

void TextureCache::pool_stats(u64& resident, u32& requests, u32& denied) const {
    resident = 0;
    for (u32 i = 0; i < count_; ++i) resident += entries_[i].texture.valid() ? entries_[i].bytes : 0;
    requests = requests_;
    denied = denied_;
}

void TextureCache::queue(u32 index) {
    Entry& e = entries_[index];
    Job job{index, "", e.srgb, 0, e.normal_map};
    if (!prepare(e, job.path, sizeof job.path, job.max_size)) {
        e.failed = true;
        return;
    }
    // Streaming by distance: only as much as its uses need now (a smaller copy, cached in DerivedData like the rest).
    const u32 size = stream && e.streams ? stream_size(e.want) : 0;
    if (size && (!job.max_size || size < job.max_size)) job.max_size = size;
    e.requested = job.max_size;
    e.pending = true;
    ++requests_;
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
        if (f.data) {
            e.complete = render::stream_load_complete(f.data->width, f.data->height, e.requested, e.cap);
            upload(e, *f.data);
        } else {
            e.failed = !e.texture.valid();
        }
        delete f.data;
    }
    // About twice a second: what each texture's draws asked for since the last look, held to the pool
    // (render::plan_texture_streaming): sharper when it fits, smaller when only far away, the least recently drawn
    // squeezed while over the pool. Each change is a load of that size (a smaller copy, cached in DerivedData).
    if (!stream || ++frame_ % 30 != 0) return;
    std::vector<render::StreamTexture> view(count_);
    std::vector<u32> sides(count_);
    for (u32 i = 0; i < count_; ++i) {
        const Entry& e = entries_[i];
        render::StreamTexture& t = view[i];
        t.side = e.failed || !e.texture.valid() ? 0u : e.side;
        t.bytes = e.texture.valid() ? e.bytes : 0;
        t.wanted = e.want ? render::stream_size(e.want) : 0u;
        if (e.cap && t.wanted > e.cap) t.wanted = e.cap;  // never asks past its cap (each such ask was a reload or a denial, every plan)
        t.last_used = e.last_used;
        t.complete = e.complete;
        t.streams = e.streams;
        t.busy = e.pending;
    }
    const render::StreamPlan plan = render::plan_texture_streaming(view.data(), count_, pool_bytes, sides.data());
    denied_ += plan.denied;
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        if (sides[i]) {
            e.want = sides[i];  // what queue() loads now (kStreamWhole is kFull)
            queue(i);
        }
        e.want = 0;
    }
}

u32 TextureCache::streaming() const {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) n += entries_[i].pending ? 1u : 0u;
    return n;
}

rhi::TextureHandle TextureCache::get(const char* guid, bool srgb, bool normal_map, u32 want) {
    if (!guid || !guid[0]) return {};
    if (want != kFull) want = static_cast<u32>(static_cast<f32>(want) * detail_scale_ * memory_scale_);
    const u32 use = srgb ? 1u : normal_map ? 2u : 0u;
    const i64 slot = index_.find(guid, use, [&](u32 i) {
        return entries_[i].srgb == srgb && entries_[i].normal_map == normal_map && std::strcmp(entries_[i].guid, guid) == 0;
    });
    if (slot >= 0) {
        Entry& seen = entries_[slot];
        seen.want = seen.want > want ? seen.want : want;  // kFull is the largest
        seen.last_used = frame_;
        return seen.texture;
    }
    if (count_ == entries_.size()) {
        if (!full_warned_) LSD_LOG_WARN("editor", "More than %zu textures in use: the rest are drawn without", entries_.size());
        full_warned_ = true;
        return {};
    }
    index_.add(guid, use, count_);
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    e.srgb = srgb;
    e.normal_map = normal_map && !srgb;
    e.want = want;
    if (stream) queue(count_ - 1);  // drawn without it for a few frames
    else load(e);
    return e.texture;
}

void TextureCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        const i64 time = e.pending ? -1 : walked_time(registry_, e.guid, true);
        char path[300];
        if (time >= 0 && time != e.modified && path_of(registry_, e.guid, path, sizeof path)) {
            LSD_LOG_INFO("editor", "Reloading texture %s", path);
            if (!e.want) e.want = e.complete ? kFull : e.side;  // back at the size it had, not the smallest
            if (stream) queue(i);  // the old one stays until the new one is up
            else load(e);
        }
    }
}

MaterialCache::Entry* MaterialCache::find(const char* guid) {
    const i64 slot = index_.find(guid, 0, [&](u32 i) { return std::strcmp(entries_[i].guid, guid) == 0; });
    if (slot >= 0) return &entries_[slot];
    if (count_ == entries_.size()) {
        if (!full_warned_) LSD_LOG_WARN("editor", "More than %zu materials in use: the rest are drawn with the default material", entries_.size());
        full_warned_ = true;
        return nullptr;
    }
    index_.add(guid, 0, count_);
    Entry& e = entries_[count_++];
    e = Entry{};
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    load(e);
    return &e;
}

bool find_texture_guid(const asset::AssetRegistry& registry, const char* name, char out[33]) {
    out[0] = '\0';
    // The exact name, else one whose name contains it (as before: "Brick" finds "T_Brick").
    if (const asset::AssetRecord* r = registry.best_match(name, asset::AssetType::Texture)) asset::to_string(r->guid, out);
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
        if (text.find("post_main") != std::string::npos && text.find("material_main") == std::string::npos) {  // a post-process material: not for surfaces
            e.failed = true;
            return;
        }
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
    const auto started = std::chrono::steady_clock::now();
    e.shader = device_.create_material_shader(code.code.data());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
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
    LSD_LOG_INFO("material", shader_asset ? "Compiled shader %s (%.0f ms)" : "Compiled material graph %s (%.0f ms)", path, ms);
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
        pack_maps(e);
        return;
    }
    e.failed = !asset::load_data_asset(path, e.material);
    if (!e.failed) pack_maps(e);
}

void MaterialCache::pack_maps(Entry& e) {
    render::Material& m = e.material;
    const char* separate[4] = {m.ambient_occlusion_texture, m.roughness_texture, m.metallic_texture, m.specular_texture};  // R, G, B, A
    if (!separate[0][0] && !separate[1][0] && !separate[2][0] && !separate[3][0]) return;
    char paths[5][300] = {};
    const char* guids[5] = {separate[0], separate[1], separate[2], separate[3], m.orm_texture};
    std::string signature;
    for (u32 i = 0; i < 5; ++i) {
        if (guids[i][0] && path_of(registry_, guids[i], paths[i], sizeof paths[i])) signature += std::string(paths[i]) + std::to_string(file_time(paths[i])) + ";";
        else signature += ";";
    }
    char key[33];
    std::snprintf(key, sizeof key, "P%016llx", static_cast<unsigned long long>(hash64(signature)));
    const std::filesystem::path out = std::filesystem::path("DerivedData") / "Materials" / (std::string(key) + ".png");
    std::error_code error;
    if (!std::filesystem::exists(out, error)) {
        std::filesystem::create_directories(out.parent_path(), error);
        asset::Image images[5];
        for (u32 i = 0; i < 5; ++i) {
            if (paths[i][0] && !asset::load_image(paths[i], images[i])) LSD_LOG_WARN("material", "Could not read %s for packing", paths[i]);
        }
        auto source = [&](u32 own, u32 channel) {  // the separate map, else the ORM texture's own channel, else white
            asset::PackSource s{};
            if (images[own].pixels) s.image = &images[own], s.channel = 0;
            else if (images[4].pixels && own < 3) s.image = &images[4], s.channel = channel;
            return s;
        };
        if (!asset::pack_channels(out.string().c_str(), source(0, 0), source(1, 1), source(2, 2), source(3, 3))) {
            LSD_LOG_WARN("material", "Could not pack the maps of material %s", e.guid);
            return;
        }
    }
    textures_.set_file(key, out.generic_string().c_str());
    std::snprintf(m.orm_texture, sizeof m.orm_texture, "%s", key);
}

f32 MaterialCache::box_uv_tiling(const char* guid) {
    const Entry* e = guid && guid[0] ? find(guid) : nullptr;
    return e && !e->failed && e->material.box_uv_tiling > 0.0f ? e->material.box_uv_tiling : 1.0f;
}

void MaterialCache::apply(const char* guid, rhi::MeshDraw& draw, u32 want) {
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
            *slots[i] = graph->texture_guids[i][0] ? textures_.get(graph->texture_guids[i], graph->texture_srgb[i], false, want) : rhi::TextureHandle{};
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
    draw.specular = m.specular;
    draw.hair = m.shading_model == 1;
    draw.emissive[0] = m.emissive.r * m.emissive_strength;
    draw.emissive[1] = m.emissive.g * m.emissive_strength;
    draw.emissive[2] = m.emissive.b * m.emissive_strength;
    draw.base_color_texture = textures_.get(m.base_color_texture, true, false, want);
    draw.normal_texture = textures_.get(m.normal_texture, false, true, want);
    draw.orm_texture = textures_.get(m.orm_texture, false, false, want);
    draw.emissive_texture = textures_.get(m.emissive_texture, true, false, want);
    // Blend Mode: Opaque, Masked (cut-outs), Translucent, Additive; Opacity scales the alpha.
    if (m.blend_mode != 0) draw.color[3] *= m.opacity;
    draw.blend = m.blend_mode == 2 ? 1u : m.blend_mode == 3 ? 2u : 0u;
    draw.alpha_cutoff = m.blend_mode == 1 ? (m.opacity_mask_clip > 0.0f ? m.opacity_mask_clip : 0.001f) : 0.0f;
}

void MaterialCache::reload_all() {
    for (u32 i = 0; i < count_; ++i) load(entries_[i]);
}

void MaterialCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        Entry& e = entries_[i];
        const i64 time = walked_time(registry_, e.guid, false);
        if (time < 0) continue;
        const i64 parent_time = e.parent[0] ? walked_time(registry_, e.parent, false) : -1;
        const bool parent_changed = parent_time >= 0 && parent_time != e.parent_modified;
        if (time != e.modified || parent_changed) load(e);
        else if (e.shader.valid()) resolve_textures(e);  // a texture imported after the graph was compiled
    }
}

} // namespace lsd::editor
