#pragma once
// Skinned models and their animations for SkeletalMesh components, keyed by asset GUID: the model
// (skeleton + GPU buffers) from a rigged FBX, and the clips of any FBX sampled onto that skeleton.
// Reloaded when the files change. The animation player (animation_player.h) samples them.
#include "lsd/animation/anim_graph.h"
#include "lsd/asset/asset_meta.h"
#include "lsd/asset/asset_registry.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <vector>

namespace lsd::editor {

struct SkinnedEntry {
    char guid[33];
    anim::SkinnedModel* model = nullptr;
    rhi::BufferHandle vertices, indices;  // indices: every section (shadows, one draw)
    u32 index_count = 0;
    // One draw per material slot: its triangles' indices and the slot's material from the mesh's
    // .meta (Material Slots; "" = none), refreshed when the .meta changes.
    static constexpr u32 kMaxLods = 4;
    struct Section {
        rhi::BufferHandle indices;
        u32 index_count = 0;
        char slot_material[33] = "";
        rhi::BufferHandle lod_indices[kMaxLods];  // [1..]: simpler versions over the same vertices (index 0 unused)
        u32 lod_index_count[kMaxLods] = {};
    };
    u32 lod_count = 1;                   // auto LODs (sections with 512+ triangles); 1 = none
    f32 lod_screen_size[kMaxLods] = {};  // [1..3]: from the mesh's import settings (LOD category)
    // The LOD for an object `radius_over_distance` across on screen (bounding radius over distance), as static meshes pick.
    u32 pick_lod(f32 radius_over_distance) const {
        u32 lod = 0;
        while (lod + 1 < lod_count && radius_over_distance < lod_screen_size[lod + 1]) ++lod;
        return lod;
    }
    // Section `s` at `lod`: that LOD's indices, or the nearest finer one the section has (LOD 0: its own).
    void section_at(u32 s, u32 lod, rhi::BufferHandle& out, u32& count) const {
        out = sections[s].indices;
        count = sections[s].index_count;
        for (u32 l = lod; l > 0; --l) {
            if (!sections[s].lod_index_count[l]) continue;
            out = sections[s].lod_indices[l];
            count = sections[s].lod_index_count[l];
            return;
        }
    }
    Section sections[asset::kMaxMaterialSlots];
    u32 section_count = 0;
    i64 meta_modified = 0;
    i64 modified = 0;
    bool failed = false;
    bool unreal_rig = false;  // UE4 / UE5 bone names: its front points down -Y in the file
    List<asset::RetargetBoneTurn, 64> retarget_pose;  // from the .meta: clips from other files retarget in it
};

// The turn (degrees about Z) that makes a skeletal mesh face its object's +X: its Facing setting,
// Auto = Unreal rigs turned +90 (their front is -Y), others not turned.
f32 facing_yaw(const world::SkeletalMesh& mesh, const SkinnedEntry* model);
// The material section `section` of `model` is drawn with on this object: its override in Material
// Overrides, else the object's one Material, else the slot's material; "" = none (the object's colour).
const char* section_material(const world::SkeletalMesh& mesh, const SkinnedEntry& model, u32 section);

// Where a skeletal mesh is drawn: its object's matrix, turned by facing_yaw. Use it (not
// world_matrix) for drawing, picking, sockets and IK.
Mat4 skeletal_matrix(const world::World& world, world::Entity e, const world::SkeletalMesh& mesh, const SkinnedEntry* model);
// A joint's frame without its scale: rigs in centimetres scale every joint by 0.01, and socket offsets
// (metres) and objects attached to a bone must not shrink with it.
Mat4 without_scale(Mat4 m);

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
    const ClipSet* set_of(const anim::Clip* clip) const;  // the loaded set holding `clip` (its .meta: curves, notifies)
    const anim::AnimGraph* graph(const char* guid);  // an AnimGraph data asset; nullptr if missing
    void check_files();  // about once a second: reload changed files and notifies
    const asset::AssetRegistry& registry() const { return registry_; }

private:
    bool load(SkinnedEntry& e);
    void load_slots(SkinnedEntry& e);  // the sections' materials from the mesh's .meta
    void load(ClipSet& set, const SkinnedEntry& model);

    rhi::Device& device_;
    const asset::AssetRegistry& registry_;
    // shortcut: a fixed 1,024 slots on the heap, never moved (get hands out pointers); grow by chunks if a project needs more.
    std::vector<SkinnedEntry> entries_ = std::vector<SkinnedEntry>(1024);
    u32 count_ = 0;
    bool full_warned_ = false;
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
