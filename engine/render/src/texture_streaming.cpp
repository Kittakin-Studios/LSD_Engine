#include "lsd/render/texture_streaming.h"

namespace lsd::render {

u32 stream_size(u32 want) {
    if (want == kStreamWhole) return kStreamWhole;
    u32 size = kStreamMinSide;
    while (size < want && size < 16384) size *= 2;
    return size;
}

namespace {

// Its bytes at `side` instead of `t.side` (mips scale with the area); all of it: guessed at twice the side.
u64 bytes_at(const StreamTexture& t, u32 side) {
    const double ratio = side == kStreamWhole ? 2.0 : static_cast<double>(side) / static_cast<double>(t.side);
    return static_cast<u64>(static_cast<double>(t.bytes) * ratio * ratio);
}

} // namespace

StreamPlan plan_texture_streaming(const StreamTexture* textures, u32 count, u64 pool_bytes, u32* out) {
    StreamPlan plan;
    for (u32 i = 0; i < count; ++i) {
        out[i] = 0;
        plan.resident_bytes += textures[i].bytes;
    }
    u64 resident = plan.resident_bytes;
    // Over the pool: halve the least recently drawn (then the largest) until it fits, one step each per plan.
    while (pool_bytes && resident > pool_bytes) {
        u32 pick = count;
        for (u32 i = 0; i < count; ++i) {
            const StreamTexture& t = textures[i];
            if (out[i] || t.busy || !t.streams || t.side <= kStreamMinSide) continue;
            const StreamTexture* best = pick < count ? &textures[pick] : nullptr;
            if (!best || t.last_used < best->last_used || (t.last_used == best->last_used && t.bytes > best->bytes)) pick = i;
        }
        if (pick == count) break;  // nothing left to squeeze
        const StreamTexture& t = textures[pick];
        const u32 half = t.side / 2 > kStreamMinSide ? t.side / 2 : kStreamMinSide;
        out[pick] = t.wanted && t.wanted < half ? t.wanted : half;  // straight to what the view needs when that is less
        resident -= t.bytes - bytes_at(t, out[pick]);
        ++plan.squeezed;
    }
    // Smaller first (what they free is room for the sharper ones below), then sharper.
    for (u32 i = 0; i < count; ++i) {
        const StreamTexture& t = textures[i];
        if (out[i] || t.busy || !t.side) continue;
        if (!t.streams) {  // Mip Streaming off: all of it, whatever the pool
            if (!t.complete) out[i] = kStreamWhole;
            continue;
        }
        // Drawn only far away now: smaller (once a quarter is enough, so it does not flicker). Not drawn since the last
        // plan: keeps what it has (the pool takes it when short).
        if (t.wanted && t.wanted != kStreamWhole && t.wanted * 4 <= t.side) {
            out[i] = t.wanted;
            resident -= t.bytes - bytes_at(t, t.wanted);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const StreamTexture& t = textures[i];
        if (out[i] || t.busy || !t.side || !t.streams || t.complete || t.wanted <= t.side) continue;
        // What it asks for if that fits, else one step, else not now.
        // shortcut: first come first served among sharper requests, rank them by screen size if pools run tight in real levels.
        const u32 steps[2] = {t.wanted, t.side * 2};
        for (u32 side : steps) {
            const u64 grown = bytes_at(t, side);
            if (pool_bytes && resident - t.bytes + grown > pool_bytes) continue;
            out[i] = side;
            resident = resident - t.bytes + grown;
            break;
        }
        if (!out[i]) ++plan.denied;
    }
    for (u32 i = 0; i < count; ++i) plan.loads += out[i] ? 1u : 0u;
    return plan;
}

} // namespace lsd::render
