#include "skinned_cache.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

#include <cmath>
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

const asset::AssetRecord* record_of(const asset::AssetRegistry& registry, const char* guid_text) {
    asset::Guid guid;
    return guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
}

} // namespace

static void destroy_buffers(rhi::Device& device, SkinnedEntry& e) {
    if (e.vertices.valid()) device.destroy_buffer(e.vertices);
    if (e.indices.valid()) device.destroy_buffer(e.indices);
    for (u32 s = 0; s < e.section_count; ++s) {
        if (e.sections[s].indices.valid()) device.destroy_buffer(e.sections[s].indices);
    }
    e.vertices = e.indices = {};
    e.section_count = 0;
}

SkinnedCache::~SkinnedCache() {
    for (u32 i = 0; i < count_; ++i) {
        destroy_buffers(device_, entries_[i]);
        delete entries_[i].model;
    }
    for (u32 i = 0; i < set_count_; ++i) delete sets_[i];
    for (u32 i = 0; i < graph_count_; ++i) delete graphs_[i];
}

void SkinnedCache::load(GraphEntry& e) {
    e.graph = anim::AnimGraph{};
    const asset::AssetRecord* r = record_of(registry_, e.guid);
    char path[300] = "";
    if (r) registry_.source_path(*r, path, sizeof path);
    e.modified = r ? file_time(path) : 0;
    e.failed = !r || !asset::load_data_asset(path, e.graph);
}

const anim::AnimGraph* SkinnedCache::graph(const char* guid) {
    for (u32 i = 0; i < graph_count_; ++i) {
        if (std::strcmp(graphs_[i]->guid, guid) == 0) return graphs_[i]->failed ? nullptr : &graphs_[i]->graph;
    }
    if (graph_count_ == sizeof graphs_ / sizeof graphs_[0]) return nullptr;
    GraphEntry* e = graphs_[graph_count_++] = new GraphEntry();
    std::snprintf(e->guid, sizeof e->guid, "%s", guid);
    load(*e);
    return e->failed ? nullptr : &e->graph;
}

bool SkinnedCache::load(SkinnedEntry& e) {
    destroy_buffers(device_, e);
    e.failed = true;
    const asset::AssetRecord* r = record_of(registry_, e.guid);
    if (!r || r->type != asset::AssetType::Mesh) return false;
    char path[300];
    registry_.source_path(*r, path, sizeof path);
    e.modified = file_time(path);
    asset::AssetMeta meta;
    registry_.load_meta(*r, meta);
    if (!e.model) e.model = new anim::SkinnedModel(default_allocator());
    char error[256];
    if (!anim::import_skinned(path, meta.import_scale, *e.model, error, sizeof error, meta.normals, meta.smoothing_angle)) return false;  // a static mesh: not ours
    e.vertices = device_.create_buffer(rhi::BufferUsage::Vertex, e.model->vertices.data(), e.model->vertices.size() * sizeof(rhi::SkinnedVertex));
    e.indices = device_.create_buffer(rhi::BufferUsage::Index, e.model->indices.data(), e.model->indices.size() * sizeof(u32));
    e.index_count = static_cast<u32>(e.model->indices.size());
    e.unreal_rig = anim::model_rig(*e.model) != anim::Rig::Other;
    e.failed = !e.vertices.valid() || !e.indices.valid();
    // shortcut: only the first kMaxMaterialSlots materials of a file are drawn; raise the limit when a mesh needs more
    for (const anim::MeshSection& section : e.model->sections) {
        if (e.section_count == asset::kMaxMaterialSlots || !section.index_count) continue;
        SkinnedEntry::Section& s = e.sections[e.section_count++];
        s.indices = device_.create_buffer(rhi::BufferUsage::Index, e.model->indices.data() + section.first_index, section.index_count * sizeof(u32));
        s.index_count = section.index_count;
    }
    if (e.model->sections.size() > asset::kMaxMaterialSlots) LSD_LOG_WARN("anim", "%s: %zu materials, only the first %u are drawn", path, e.model->sections.size(), asset::kMaxMaterialSlots);
    load_slots(e);
    return !e.failed;
}

// Which model section a drawn section is (empty sections are skipped when the buffers are made).
static const anim::MeshSection* model_section(const SkinnedEntry& e, u32 section) {
    u32 k = 0;
    for (const anim::MeshSection& s : e.model->sections) {
        if (!s.index_count) continue;
        if (k++ == section) return &s;
    }
    return nullptr;
}

void SkinnedCache::load_slots(SkinnedEntry& e) {
    const asset::AssetRecord* r = record_of(registry_, e.guid);
    for (u32 s = 0; s < e.section_count; ++s) e.sections[s].slot_material[0] = '\0';
    if (!r || !e.model) return;
    char path[300];
    registry_.source_path(*r, path, sizeof path);
    std::snprintf(path + std::strlen(path), sizeof path - std::strlen(path), ".meta");
    e.meta_modified = file_time(path);
    asset::AssetMeta meta;
    registry_.load_meta(*r, meta);
    for (u32 s = 0; s < e.section_count; ++s) {
        const anim::MeshSection* section = model_section(e, s);
        for (u32 k = 0; section && k < meta.material_slots.count; ++k) {
            if (std::strcmp(meta.material_slots[k].name, section->material) == 0) {
                std::snprintf(e.sections[s].slot_material, sizeof e.sections[s].slot_material, "%s", meta.material_slots[k].material);
            }
        }
    }
}

const char* section_material(const world::SkeletalMesh& mesh, const SkinnedEntry& model, u32 section) {
    const anim::MeshSection* s = model_section(model, section);
    for (u32 k = 0; s && k < mesh.material_overrides.count; ++k) {
        const world::MaterialOverride& o = mesh.material_overrides[k];
        if (o.material[0] && std::strcmp(o.slot, s->material) == 0) return o.material;
    }
    if (mesh.material[0]) return mesh.material;
    return section < model.section_count ? model.sections[section].slot_material : "";
}

f32 facing_yaw(const world::SkeletalMesh& mesh, const SkinnedEntry* model) {
    switch (mesh.facing) {
        case 1: return 0.0f;     // +X: already forward
        case 2: return -90.0f;   // +Y
        case 3: return 180.0f;   // -X
        case 4: return 90.0f;    // -Y
        default: return model && model->unreal_rig ? 90.0f : 0.0f;
    }
}

Mat4 without_scale(Mat4 m) {
    for (int c = 0; c < 3; ++c) {
        const f32 length = std::sqrt(m.at(0, c) * m.at(0, c) + m.at(1, c) * m.at(1, c) + m.at(2, c) * m.at(2, c));
        if (length > 1e-8f) {
            for (int r = 0; r < 3; ++r) m.at(r, c) /= length;
        }
    }
    return m;
}

Mat4 skeletal_matrix(const world::World& world, world::Entity e, const world::SkeletalMesh& mesh, const SkinnedEntry* model) {
    const f32 yaw = facing_yaw(mesh, model);
    return yaw == 0.0f ? world.world_matrix(e) : world.world_matrix(e) * rotation_z(radians(yaw));
}

const SkinnedEntry* SkinnedCache::get(const char* guid) {
    if (!guid || !guid[0]) return nullptr;
    for (u32 i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].guid, guid) == 0) return entries_[i].failed ? nullptr : &entries_[i];
    }
    if (count_ == sizeof entries_ / sizeof entries_[0]) return nullptr;
    SkinnedEntry& e = entries_[count_++];
    std::snprintf(e.guid, sizeof e.guid, "%s", guid);
    load(e);
    return e.failed ? nullptr : &e;
}

void SkinnedCache::load(ClipSet& set, const SkinnedEntry& model) {
    const asset::AssetRecord* r = record_of(registry_, set.anim_guid);
    set.count = 0;
    if (!r) return;
    char path[300];
    registry_.source_path(*r, path, sizeof path);
    set.modified = file_time(path);
    set.meta = asset::AssetMeta{};
    registry_.load_meta(*r, set.meta);
    anim::ClipTrim trims[16];
    for (u32 i = 0; i < set.meta.clip_ranges.count; ++i) trims[i] = {set.meta.clip_ranges[i].clip, set.meta.clip_ranges[i].start, set.meta.clip_ranges[i].end};
    set.count = anim::import_clips(path, *model.model, set.clips, 16, trims, set.meta.clip_ranges.count);
}

const ClipSet* SkinnedCache::set_of(const anim::Clip* clip) const {
    for (u32 i = 0; i < set_count_; ++i) {
        if (clip >= sets_[i]->clips && clip < sets_[i]->clips + sets_[i]->count) return sets_[i];
    }
    return nullptr;
}

const ClipSet* SkinnedCache::clips(const char* mesh_guid, const char* anim_guid) {
    const SkinnedEntry* model = get(mesh_guid);
    if (!model) return nullptr;
    const char* source = anim_guid && anim_guid[0] ? anim_guid : mesh_guid;
    for (u32 i = 0; i < set_count_; ++i) {
        if (std::strcmp(sets_[i]->model_guid, mesh_guid) == 0 && std::strcmp(sets_[i]->anim_guid, source) == 0) return sets_[i];
    }
    if (set_count_ == sizeof sets_ / sizeof sets_[0]) return nullptr;
    ClipSet* set = sets_[set_count_++] = new ClipSet();
    std::snprintf(set->model_guid, sizeof set->model_guid, "%s", mesh_guid);
    std::snprintf(set->anim_guid, sizeof set->anim_guid, "%s", source);
    load(*set, *model);
    return set;
}

void SkinnedCache::check_files() {
    for (u32 i = 0; i < count_; ++i) {
        SkinnedEntry& e = entries_[i];
        const asset::AssetRecord* r = record_of(registry_, e.guid);
        char path[300];
        if (!r) continue;
        registry_.source_path(*r, path, sizeof path);
        if (file_time(path) == e.modified) {
            std::snprintf(path + std::strlen(path), sizeof path - std::strlen(path), ".meta");
            if (!e.failed && file_time(path) != e.meta_modified) load_slots(e);  // Material Slots edited
            continue;
        }
        if (load(e)) LSD_LOG_INFO("editor", "Reloaded skinned mesh %s", path);
        for (u32 k = 0; k < set_count_; ++k) {  // its clips were sampled onto the old skeleton
            if (std::strcmp(sets_[k]->model_guid, e.guid) == 0) sets_[k]->modified = -1;
        }
    }
    for (u32 i = 0; i < set_count_; ++i) {
        ClipSet& set = *sets_[i];
        const asset::AssetRecord* r = record_of(registry_, set.anim_guid);
        const SkinnedEntry* model = get(set.model_guid);
        if (!r || !model) continue;
        char path[300];
        registry_.source_path(*r, path, sizeof path);
        if (file_time(path) != set.modified) {
            load(set, *model);
            continue;
        }
        const asset::AssetMeta before = set.meta;
        registry_.load_meta(*r, set.meta);  // notifies edited in Details
        bool trimmed = before.clip_ranges.count != set.meta.clip_ranges.count;
        for (u32 k = 0; k < set.meta.clip_ranges.count && !trimmed; ++k) {
            const asset::ClipRange &a = before.clip_ranges[k], &b = set.meta.clip_ranges[k];
            trimmed = a.clip != b.clip || a.start != b.start || a.end != b.end;
        }
        if (trimmed) load(set, *model);  // its takes are cut again
    }
    for (u32 i = 0; i < graph_count_; ++i) {  // graphs edited in Details save to disk: picked up here
        const asset::AssetRecord* r = record_of(registry_, graphs_[i]->guid);
        char path[300];
        if (!r) continue;
        registry_.source_path(*r, path, sizeof path);
        if (file_time(path) != graphs_[i]->modified) load(*graphs_[i]);
    }
}

} // namespace lsd::editor
