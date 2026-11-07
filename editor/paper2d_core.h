#pragma once
// 2D (Paper2D) logic without the GPU, shared by the editor, Play and the tests: a Tile Map's collision
// from its tiles' shapes, and which frame of a Flipbook plays at a time.
#include "lsd/foundation/math.h"
#include "lsd/world/components.h"

namespace lsd::editor {

// A collision piece of a Tile Map in its local space: a box (runs of Full tiles, or of one half kind
// along a row) or one tile's ramp. 1 m deep so a character on the plane y = 0 stands on it.
struct TileRun {
    Vec3 lo, hi;
    u32 kind = 0;  // 0 box; 1 a ramp rising toward +X; 2 a ramp falling toward +X (the wedge inside lo..hi); 3 a one-way box
};
enum TileShapeKind : u32 { kTileFull, kTileNone, kTileTopHalf, kTileBottomHalf, kTileSlopeUp, kTileSlopeDown, kTileOneWay };
// The shape of tileset tile `tile` (painted value, 1-based) in `map`; Full when not set.
u32 tile_shape(const world::TileMap& map, i32 tile);
// The map's collision pieces; returns how many (at most `capacity`).
u32 tile_runs(const world::TileMap& map, TileRun* out, u32 capacity);
// The same for other tiles of the map's size (a layer's).
u32 tile_runs(const world::TileMap& map, const List<i32, 4096>& tiles, TileRun* out, u32 capacity);
// The tiles of layer `layer`: 0 the map's own, 1.. its Layers; nullptr if there is no such layer.
List<i32, 4096>* layer_tiles(world::TileMap& map, u32 layer);

// The index into fb.frames playing `seconds` after it started (looping, or held on the last frame);
// -1 when it has no frames.
i32 flipbook_frame(const world::Flipbook& fb, f32 seconds);

} // namespace lsd::editor
