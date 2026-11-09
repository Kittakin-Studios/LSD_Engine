#pragma once
// HLOD (Unreal's Hierarchical LOD): Build > Build HLODs groups the level's static meshes into cells
// (a grid of World Settings > HLOD Cell Size metres) and merges each cell's opaque meshes into a few
// simplified proxy meshes, one per look (material + tint). Far from the camera (HLOD Distance) a cell
// draws its proxies instead of its objects: a handful of draws for a whole street.
// Saved next to the level as <Level>_BuiltData.lsdhlod and loaded with it.
// shortcut: no atlas baking, so one proxy per distinct material + tint (not one per cell), no lightmaps, shadows still use the objects, stale after edits until rebuilt.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/math.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/components.h"
#include "lsd/world/world.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

// One proxy: the cell's objects with this look, merged (world space) and simplified.
struct HlodProxy {
    char material[40] = "";
    f32 color[4] = {1, 1, 1, 1};
    std::vector<rhi::MeshVertex> vertices;
    std::vector<u32> indices;
};

struct HlodCell {
    f32 lo[3] = {}, hi[3] = {};  // bounds of the members (world)
    std::vector<u64> members;    // hlod_member_key of each object the proxies stand for
    std::vector<HlodProxy> proxies;
};

struct HlodData {
    f32 cell_size = 0.0f;
    std::vector<HlodCell> cells;
    std::unordered_map<u64, u32> cell_of;  // member key -> cell (made by index())
    bool valid() const { return !cells.empty(); }
    void index();
};

// An object to merge: its mesh (object space) and where it stands.
struct HlodSource {
    u64 key = 0;
    Mat4 world;
    char material[40] = "";
    f32 color[4] = {1, 1, 1, 1};
    const rhi::MeshVertex* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
};

// Name, mesh and place (to the millimetre) as one number: finds an object again at run time.
u64 hlod_member_key(const char* name, const char* mesh, Vec3 position);
// Groups sources into cells of `cell_size` metres by their position, merges each cell's by look and keeps
// `ratio` of each mesh's triangles. Cells with fewer than 2 objects are left out (nothing to gain).
void build_hlod(const std::vector<HlodSource>& sources, f32 cell_size, f32 ratio, HlodData& out);
// True when the cell is farther than `distance` from `eye` (from its nearest point): draw its proxies.
bool hlod_cell_far(const HlodCell& cell, Vec3 eye, f32 distance);

// The level's static meshes as sources (opaque, not moving, plain or no Material, not hidden) with their mesh data
// held in `storage`. `allow` can veto an object. False when there is nothing.
struct HlodStorage {
    std::vector<std::vector<rhi::MeshVertex>> vertices;
    std::vector<std::vector<u32>> indices;
};
bool gather_hlod_sources(const world::World& world, const asset::AssetRegistry& registry, const char* data_dir, Allocator& allocator,
                         const std::function<bool(const world::StaticMesh&)>& allow, HlodStorage& storage, std::vector<HlodSource>& out);

std::string hlod_path(const char* level_path);  // Levels/Main.lsdlevel -> Levels/Main_BuiltData.lsdhlod
bool save_hlod(const char* path, const HlodData& data);
bool load_hlod(const char* path, HlodData& out);

} // namespace lsd::editor
