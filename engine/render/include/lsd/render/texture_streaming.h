#pragma once
// Texture streaming decisions (Unreal's texture streaming / its streaming pool): which mips of each texture should be
// resident, from what the view asks for and a memory budget. Pure logic (no GPU, no files); the editor's TextureCache
// gathers the numbers, calls plan_texture_streaming about twice a second and loads what it says.
#include "lsd/foundation/types.h"

namespace lsd::render {

constexpr u32 kStreamMinSide = 64;           // a streamed texture never goes smaller than this
constexpr u32 kStreamWhole = 0xFFFFFFFFu;    // load all of it (its own Max Size)

// The side a texture loads at to cover `want` texels across: a power of two, at least kStreamMinSide (kStreamWhole stays).
u32 stream_size(u32 want);

struct StreamTexture {
    u32 side = 0;           // the longer side it is resident at (0: not on the GPU yet)
    u64 bytes = 0;          // what it holds on the GPU, every mip
    u32 wanted = 0;         // stream_size of the most any draw asked for since the last plan (0: not drawn)
    u32 last_used = 0;      // when it was last drawn (any growing counter; the least recent goes first under the budget)
    bool complete = false;  // all of it is resident: it cannot get sharper
    bool streams = true;    // its Mip Streaming setting; off: always all of it
    bool busy = false;      // a load is on its way: left alone
};

struct StreamPlan {
    u32 loads = 0;           // loads asked for (sharper, smaller or squeezed)
    u32 squeezed = 0;        // of those, halved for the budget
    u32 denied = 0;          // sharper ones held back because they would not fit
    u64 resident_bytes = 0;  // before this plan
};

// One look at every texture: out[i] = the side to load texture i at now (kStreamWhole: all of it),
// or 0 to keep what it has. Drawn ones get what the view needs: sharper as soon as it fits in `pool_bytes`, smaller
// once a quarter of it is enough. While the resident total is over the pool, the least recently drawn (then the largest)
// halve, one step each plan, down to kStreamMinSide. `pool_bytes` 0: no limit. Textures that do not stream are never
// squeezed.
StreamPlan plan_texture_streaming(const StreamTexture* textures, u32 count, u64 pool_bytes, u32* out);

} // namespace lsd::render
