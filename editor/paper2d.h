#pragma once
// 2D games (Unreal's Paper2D): Sprite and Tile Map components are quads in the X-Z plane, facing -Y
// (a side-on game looks along +Y, the Front view). Their meshes are generated and cached here; the
// main draw loop, picking and Play collision use them. The 2D level mode lives in editor_modes.cpp.
#include "paper2d_core.h"
#include "lsd/foundation/math.h"
#include "lsd/world/components.h"

namespace lsd::asset {
class AssetRegistry;
}

namespace lsd::editor {

class MeshCache;
struct CachedMesh;

// The sheet cell a sprite shows `seconds` into its animation: its Flipbook asset when it has one
// (`flipbook`, from find_flipbook), else Frame / Frame Count / FPS (Frame when it is still).
u32 sprite_cell(const world::Sprite& sprite, f32 seconds, const world::Flipbook* flipbook = nullptr);
// A Flipbook asset by GUID, loaded and kept (re-read when its file changes); null when there is none.
const world::Flipbook* find_flipbook(const asset::AssetRegistry* registry, const char* guid);
// A quad for that cell: 1 m wide and high, pivot at the bottom centre, both sides (scale it with the Transform).
const CachedMesh* sprite_mesh(MeshCache& meshes, const world::Sprite& sprite, u32 cell);
// The whole map as one mesh from its local origin (bottom-left), rebuilt when a tile changes; `id`
// tells maps apart (the entity's value).
const CachedMesh* tile_map_mesh(MeshCache& meshes, const world::TileMap& map, u32 id);
// Local boxes (picking, collision): a sprite 1 x 0.1 x 1 m; a map its tiles, 0.1 m deep.
void sprite_bounds(Vec3& lo, Vec3& hi);
// A world UI panel (UiPanel): a width x height quad centred on its origin, facing -Y, both sides; `flip_v`
// when the device's render targets come out upside down (OpenGL). Its box is 0.02 m deep.
const CachedMesh* ui_panel_mesh(MeshCache& meshes, const world::UiPanel& panel, bool flip_v);
void ui_panel_bounds(const world::UiPanel& panel, Vec3& lo, Vec3& hi);
void tile_map_bounds(const world::TileMap& map, Vec3& lo, Vec3& hi);

} // namespace lsd::editor
