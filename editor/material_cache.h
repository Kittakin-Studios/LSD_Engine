#pragma once
// GPU textures and loaded materials for drawing, keyed by asset GUID. Both reload when their
// files change (check_files, about once a second), so edits in Photoshop or the Details panel
// show up live.
#include "lsd/asset/asset_registry.h"
#include "lsd/render/material.h"
#include "lsd/rhi/rhi.h"

namespace lsd::editor {

// The GUID of the texture asset named `name` (an exact name wins over a partial match); false if none.
bool find_texture_guid(const asset::AssetRegistry& registry, const char* name, char out[33]);

class TextureCache {
public:
    TextureCache(rhi::Device& device, const asset::AssetRegistry& registry) : device_(device), registry_(registry) {}
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // `srgb` is how the use wants it (colour vs data); the same file may be loaded both ways.
    // Invalid if the asset is missing or not an image.
    rhi::TextureHandle get(const char* guid, bool srgb);
    void check_files();
    rhi::Device& device() { return device_; }
    // The largest side any texture loads at (0: as each asset says); video memory sets it.
    void set_size_cap(u32 size) { size_cap_ = size; }

private:
    struct Entry {
        char guid[33];
        bool srgb;
        rhi::TextureHandle texture;
        i64 modified;
        bool failed;
    };
    bool load(Entry& entry);

    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    u32 size_cap_ = 0;
    Entry entries_[512];
    u32 count_ = 0;
};

class MaterialCache {
public:
    MaterialCache(TextureCache& textures, const asset::AssetRegistry& registry) : textures_(textures), device_(textures.device()), registry_(registry) {}

    // Fills the material part of a draw (the draw's colour is the tint and is multiplied in). No
    // material: the draw keeps its colour with default roughness. A Material Graph sets the draw's
    // material shader and its textures (compiled when first used and again when its file changes).
    void apply(const char* material_guid, rhi::MeshDraw& draw);
    void check_files();

private:
    struct Entry {
        char guid[33];
        render::Material material;  // an instance's parent with its overrides applied
        i64 modified;
        char parent[33];             // instances: the parent material, reloaded when it changes
        i64 parent_modified;
        bool failed;
        rhi::MaterialShaderHandle shader;  // Material Graphs
        char textures[3][64];              // their texture asset names, and the GUIDs found for them
        char texture_guids[3][33];
        bool texture_srgb[3];
        u32 texture_count;
        u32 blend_mode;  // a graph's Material Output Blend Mode
        // Graphs: their parameters' defaults; instances of a graph: those with the instance's values.
        f32 params[16][4];
        char param_names[16][32];
        u32 param_count;
        bool graph_instance;  // a MaterialInstance whose parent (`parent`) is a Material Graph
    };
    void load_graph(Entry& entry, const char* path);
    void resolve_textures(Entry& entry);
    Entry* find(const char* guid);
    void load(Entry& entry);

    TextureCache& textures_;
    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    Entry entries_[256];
    u32 count_ = 0;
};

} // namespace lsd::editor
