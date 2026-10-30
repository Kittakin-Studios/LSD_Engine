#pragma once
// Skinned models and their animations for SkeletalMesh components, keyed by asset GUID: the model
// (skeleton + GPU buffers) from a rigged FBX, and the clips of any FBX sampled onto that skeleton.
// Reloaded when the files change. The animation player (animation_player.h) samples them.
#include "lsd/animation/anim_graph.h"
#include "lsd/asset/asset_meta.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/rhi/rhi.h"

namespace lsd::editor {

struct SkinnedEntry {
    char guid[33];
    anim::SkinnedModel* model = nullptr;
    rhi::BufferHandle vertices, indices;
    u32 index_count = 0;
    i64 modified = 0;
    bool failed = false;
};

// One animation file's clips on one model's skeleton, plus its notifies (from its .meta).
struct ClipSet {
    char model_guid[33];
    char anim_guid[33];
    anim::Clip clips[16];
    u32 count = 0;
    i64 modified = 0;
    asset::AssetMeta meta;  // notifies
};

class SkinnedCache {
public:
    SkinnedCache(rhi::Device& device, const asset::AssetRegistry& registry) : device_(device), registry_(registry) {}
    ~SkinnedCache();
    SkinnedCache(const SkinnedCache&) = delete;
    SkinnedCache& operator=(const SkinnedCache&) = delete;

    // nullptr if the asset is not a rigged mesh (or cannot be read).
    const SkinnedEntry* get(const char* mesh_guid);
    // The clips of `anim_guid` (empty: the mesh's own file) on the mesh's skeleton; nullptr if none.
    const ClipSet* clips(const char* mesh_guid, const char* anim_guid);
    const anim::AnimGraph* graph(const char* guid);  // an AnimGraph data asset; nullptr if missing
    void check_files();  // about once a second: reload changed files and notifies

private:
    bool load(SkinnedEntry& e);
    void load(ClipSet& set, const SkinnedEntry& model);

    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    SkinnedEntry entries_[64];
    u32 count_ = 0;
    ClipSet* sets_[64] = {};
    u32 set_count_ = 0;
    struct GraphEntry {
        char guid[33];
        anim::AnimGraph graph;
        i64 modified = 0;
        bool failed = false;
    };
    void load(GraphEntry& e);
    GraphEntry* graphs_[32] = {};
    u32 graph_count_ = 0;
};

} // namespace lsd::editor
