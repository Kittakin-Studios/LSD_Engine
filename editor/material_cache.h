#pragma once
// GPU textures and loaded materials for drawing, keyed by asset GUID. Both reload when their
// files change (check_files, about once a second), so edits in Photoshop or the Details panel
// show up live.
#include "key_index.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/asset/texture.h"
#include "lsd/render/material.h"
#include "lsd/rhi/rhi.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lsd::editor {

// The GUID of the texture asset named `name` (an exact name wins over a partial match); false if none.
bool find_texture_guid(const asset::AssetRegistry& registry, const char* name, char out[33]);

// Texture streaming (Unreal's, its loading part): a texture asked for the first time is read and
// decoded on a worker thread while drawing goes on without it (its material's plain values), and
// update() puts a few finished ones on the GPU each frame, so opening a level never waits for images.
class TextureCache {
public:
    TextureCache(rhi::Device& device, const asset::AssetRegistry& registry);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // `srgb` is how the use wants it (colour vs data); the same file may be loaded both ways.
    // Invalid if the asset is missing or not an image, and while it is still streaming in.
    // `normal_map`: a normal map (linear), compressed to BC5 when compressing.
    // `want`: how many texels across the use needs it (its size on screen; texture streaming by distance loads only
    // that much, at least 64, more when it comes closer); kFull: all of it.
    static constexpr u32 kFull = 0xFFFFFFFFu;
    rhi::TextureHandle get(const char* guid, bool srgb, bool normal_map = false, u32 want = kFull);
    void check_files();
    // A texture made on the fly (a Material's packed maps): `key` stands for the file at `path` in get().
    void set_file(const char* key, const char* path) { files_[key] = path; }
    // Once a frame: uploads up to `budget` textures the worker finished.
    void update(u32 budget = 4);
    u32 streaming() const;  // textures still on their way
    // Off (r.TextureStreaming 0): get() loads at once, as before (screenshots of the first frames).
    bool stream = true;
    rhi::Device& device() { return device_; }
    // The largest side any texture loads at (0: as each asset says); video memory sets it.
    void set_size_cap(u32 size) { size_cap_ = size; }
    // Texture quality: times what get() is asked for (scalability's texture_detail_scale).
    void set_detail_scale(f32 scale) { detail_scale_ = scale; }
    // Video memory pressure while running (1 = none): times the detail too, halved when the budget runs short.
    void set_memory_scale(f32 scale) { memory_scale_ = scale; }
    f32 memory_scale() const { return memory_scale_; }
    f32 memory_scale_ = 1.0f;
    // The texture pool (Unreal's texture streaming pool, r.TexturePoolSize): streamed textures stay within this many bytes
    // on the GPU, the least recently drawn giving up their sharpest mips first (0: no limit).
    u64 pool_bytes = 0;
    u32 loaded_size(rhi::TextureHandle texture) const;  // the largest side it was loaded at (0: unknown); tests and stat
    // stat dump: textures on the GPU, how many are loaded smaller than their file (streamed down), the texels they hold.
    void stats(u32& textures, u32& reduced, u64& texels) const;
    // stat dump: bytes on the GPU (every mip), loads asked for so far, sharper loads the pool held back so far.
    void pool_stats(u64& resident, u32& requests, u32& denied) const;
    f32 detail_scale_ = 1.0f;

private:
    std::map<std::string, std::string> files_;  // shortcut: keys of packed maps stay for the run (a few per material)
    const bool compress_;  // colour textures as BC1 / BC3, normal maps as BC5 (the device takes them): 4 to 8 times less video memory
    struct Entry {
        char guid[33];
        bool srgb;
        bool normal_map;
        rhi::TextureHandle texture;
        i64 modified;
        bool failed;
        bool pending;  // queued or decoding on the worker
        // Streaming by distance: the most asked for since the last look (kFull: all), the cap it was last loaded with
        // (0: none, the whole image) and whether that was the whole image anyway.
        u32 want = 0;
        u32 requested = 0;
        bool complete = false;
        u64 loaded_texels = 0;  // of its largest mip, for stat dump
        u32 side = 0;           // its longer side on the GPU now
        u64 bytes = 0;          // and what it holds there, every mip
        u32 last_used = 0;      // frame it was last asked for (the pool squeezes the least recent first)
        bool streams = true;    // its .meta's Mip Streaming
    };
    u32 frame_ = 0;
    u32 requests_ = 0, denied_ = 0;
    static u32 stream_size(u32 want);  // 0: the whole image
    bool load(Entry& entry);
    // Path, time and size for loading `e` (main thread: the registry is not shared with the worker); false if it has no file.
    bool prepare(Entry& e, char* path, usize capacity, u32& max_size);
    void queue(u32 entry);
    void upload(Entry& e, const asset::TextureData& data);

    struct Job {
        u32 entry;
        char path[300];
        bool srgb;
        u32 max_size;
        bool normal_map;
    };
    struct Finished {
        u32 entry;
        asset::TextureData* data;  // null: the file could not be read
    };
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Job> jobs_;
    std::vector<Finished> finished_;
    bool quit_ = false;

    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    u32 size_cap_ = 0;
    // shortcut: a fixed 16,384 slots on the heap, never moved (the worker reads them by index); grow by chunks if a project needs more.
    std::vector<Entry> entries_ = std::vector<Entry>(16384);
    u32 count_ = 0;
    bool full_warned_ = false;
    KeyIndex index_;  // guid (salted by colour / data use) -> slot
};

class MaterialCache {
public:
    MaterialCache(TextureCache& textures, const asset::AssetRegistry& registry) : textures_(textures), device_(textures.device()), registry_(registry) {}

    // Fills the material part of a draw (the draw's colour is the tint and is multiplied in). No
    // material: the draw keeps its colour with default roughness. A Material Graph sets the draw's
    // material shader and its textures (compiled when first used and again when its file changes).
    // `want`: texels across its textures should have (TextureCache::get); kFull for all of them.
    void apply(const char* material_guid, rhi::MeshDraw& draw, u32 want = TextureCache::kFull);
    TextureCache& textures() { return textures_; }
    void check_files();
    void reload_all();  // every material compiled again (the global material parameters were renamed or reordered)

private:
    struct Entry {
        char guid[33];
        render::Material material;  // an instance's parent with its overrides applied
        i64 modified;
        char parent[33];             // instances: the parent material, reloaded when it changes
        i64 parent_modified;
        bool failed;
        rhi::MaterialShaderHandle shader;  // Material Graphs
        char textures[4][64];              // their texture asset names, and the GUIDs found for them
        char texture_guids[4][33];
        bool texture_srgb[4];
        u32 texture_count;
        u32 blend_mode;  // a graph's Material Output Blend Mode
        // Graphs: their parameters' defaults; instances of a graph: those with the instance's values.
        f32 params[16][4];
        char param_names[16][32];
        u32 param_count;
        bool graph_instance;  // a MaterialInstance whose parent (`parent`) is a Material Graph
    };
    void load_graph(Entry& entry, const char* path);
    // Separate Roughness / Metallic / Ambient Occlusion / Specular maps packed into one ORM texture (R, G, B, A) when the material loads.
    void pack_maps(Entry& entry);
    void resolve_textures(Entry& entry);
    Entry* find(const char* guid);
    void load(Entry& entry);

    TextureCache& textures_;
    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    // shortcut: a fixed 8,192 slots on the heap, never moved (find hands out pointers); grow by chunks if a project needs more.
    std::vector<Entry> entries_ = std::vector<Entry>(8192);
    u32 count_ = 0;
    bool full_warned_ = false;
    KeyIndex index_;  // guid -> slot
};

} // namespace lsd::editor
