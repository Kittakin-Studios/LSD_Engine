#include "paper2d_core.h"

#include <cmath>

namespace lsd::editor {

u32 tile_shape(const world::TileMap& map, i32 tile) {
    if (tile <= 0) return kTileNone;
    const u32 cell = static_cast<u32>(tile - 1);
    return cell < map.tile_shapes.count ? map.tile_shapes[cell].shape : kTileFull;
}

u32 tile_runs(const world::TileMap& map, TileRun* out, u32 capacity) { return tile_runs(map, map.tiles, out, capacity); }

List<i32, 4096>* layer_tiles(world::TileMap& map, u32 layer) {
    if (layer == 0) return &map.tiles;
    return layer - 1 < map.layers.count ? &map.layers[layer - 1].tiles : nullptr;
}

u32 tile_runs(const world::TileMap& map, const List<i32, 4096>& tiles, TileRun* out, u32 capacity) {
    u32 n = 0;
    const f32 s = map.tile_size;
    for (u32 y = 0; y < map.height; ++y) {
        auto shape_at = [&](u32 at) {
            const u32 i = y * map.width + at;
            return i < tiles.count ? tile_shape(map, tiles[i]) : static_cast<u32>(kTileNone);
        };
        u32 x = 0;
        while (x < map.width && n < capacity) {
            const u32 shape = shape_at(x);
            if (shape == kTileNone) {
                ++x;
                continue;
            }
            const f32 y0 = static_cast<f32>(y) * s;
            if (shape == kTileSlopeUp || shape == kTileSlopeDown) {  // ramps one tile at a time
                out[n++] = {{static_cast<f32>(x) * s, -0.5f, y0}, {static_cast<f32>(x + 1) * s, 0.5f, y0 + s}, shape == kTileSlopeUp ? 1u : 2u};
                ++x;
                continue;
            }
            const u32 start = x;  // a run of the same box shape
            while (x < map.width && shape_at(x) == shape) ++x;
            const f32 lo = shape == kTileTopHalf ? y0 + s * 0.5f : y0, hi = shape == kTileBottomHalf ? y0 + s * 0.5f : y0 + s;
            out[n++] = {{static_cast<f32>(start) * s, -0.5f, lo}, {static_cast<f32>(x) * s, 0.5f, hi}, shape == kTileOneWay ? 3u : 0u};
        }
    }
    return n;
}

i32 flipbook_frame(const world::Flipbook& fb, f32 seconds) {
    if (!fb.frames.count) return -1;
    f32 total = 0.0f;
    for (u32 i = 0; i < fb.frames.count; ++i) total += fb.frames[i].duration > 0.001f ? fb.frames[i].duration : 0.001f;
    f32 t = seconds < 0.0f ? 0.0f : seconds;
    if (fb.loop) t = std::fmod(t, total);
    else if (t >= total) return static_cast<i32>(fb.frames.count) - 1;
    for (u32 i = 0; i < fb.frames.count; ++i) {
        t -= fb.frames[i].duration > 0.001f ? fb.frames[i].duration : 0.001f;
        if (t < 0.0f) return static_cast<i32>(i);
    }
    return static_cast<i32>(fb.frames.count) - 1;
}

} // namespace lsd::editor
