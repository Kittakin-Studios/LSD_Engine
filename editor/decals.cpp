#include "decals.h"

#include "lighting.h"

#include "lsd/asset/texture.h"
#include "lsd/world/components.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lsd::editor {

DecalAtlas::~DecalAtlas() {
    if (atlas_.valid()) device_.destroy_texture(atlas_);
}

i32 DecalAtlas::tile_of(const char* guid) const {
    for (u32 i = 0; i < count_; ++i) {
        if (std::strcmp(keys_[i], guid) == 0) return static_cast<i32>(i);
    }
    return -1;
}

void DecalAtlas::update(const world::World& world) {
    // The textures the level uses now, with their file times.
    char wanted[kTiles * kTiles][33] = {};
    i64 times[kTiles * kTiles] = {};
    u32 n = 0;
    auto want = [&](const char* texture) {
        asset::Guid g;
        const asset::AssetRecord* r = asset::parse_guid(texture, g) ? registry_.find(g) : nullptr;
        if (!r || r->type != asset::AssetType::Texture || n == kTiles * kTiles) return;
        for (u32 k = 0; k < n; ++k) {
            if (std::strcmp(wanted[k], texture) == 0) return;
        }
        std::snprintf(wanted[n], sizeof wanted[n], "%s", texture);
        times[n++] = r->source_time;
    };
    const reflect::TypeInfo& type = reflect::type_of<world::Decal>();
    for (u32 i = 0; i < world.count(type); ++i) want(static_cast<const world::Decal*>(world.at(type, i))->texture);
    // light functions share the tiles
    const reflect::TypeInfo& spot_type = reflect::type_of<world::SpotLight>();
    for (u32 i = 0; i < world.count(spot_type); ++i) want(static_cast<const world::SpotLight*>(world.at(spot_type, i))->light_function);
    const reflect::TypeInfo& rect_type = reflect::type_of<world::RectLight>();
    for (u32 i = 0; i < world.count(rect_type); ++i) want(static_cast<const world::RectLight*>(world.at(rect_type, i))->light_function);
    bool same = n == count_;
    for (u32 i = 0; i < n && same; ++i) same = std::strcmp(wanted[i], keys_[i]) == 0 && times[i] == times_[i];
    if (same) return;
    // Rebuild: each texture squeezed into its 256 x 256 tile (bilinear).
    const u32 size = kTile * kTiles;
    std::vector<u8> pixels(static_cast<size_t>(size) * size * 4, 0);
    for (u32 i = 0; i < n; ++i) {
        asset::Guid g;
        asset::parse_guid(wanted[i], g);
        char path[300];
        registry_.source_path(*registry_.find(g), path, sizeof path);
        asset::TextureData data(allocator_);
        if (!asset::load_texture(path, true, 512, data) || !data.width || !data.height) continue;
        const u32 ox = (i % kTiles) * kTile, oy = (i / kTiles) * kTile;
        for (u32 y = 0; y < kTile; ++y) {
            for (u32 x = 0; x < kTile; ++x) {
                const f32 sx = (static_cast<f32>(x) + 0.5f) * static_cast<f32>(data.width) / kTile - 0.5f;
                const f32 sy = (static_cast<f32>(y) + 0.5f) * static_cast<f32>(data.height) / kTile - 0.5f;
                const i32 x0 = std::clamp(static_cast<i32>(sx), 0, static_cast<i32>(data.width) - 1), y0 = std::clamp(static_cast<i32>(sy), 0, static_cast<i32>(data.height) - 1);
                const i32 x1 = std::min(x0 + 1, static_cast<i32>(data.width) - 1), y1 = std::min(y0 + 1, static_cast<i32>(data.height) - 1);
                const f32 fx = std::clamp(sx - static_cast<f32>(x0), 0.0f, 1.0f), fy = std::clamp(sy - static_cast<f32>(y0), 0.0f, 1.0f);
                for (u32 c = 0; c < 4; ++c) {
                    auto at = [&](i32 px, i32 py) { return static_cast<f32>(data.pixels[(static_cast<size_t>(py) * data.width + static_cast<size_t>(px)) * 4 + c]); };
                    const f32 v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                    pixels[((static_cast<size_t>(oy + y)) * size + ox + x) * 4 + c] = static_cast<u8>(v + 0.5f);
                }
            }
        }
    }
    if (atlas_.valid()) device_.destroy_texture(atlas_);
    const void* mips[1] = {pixels.data()};
    atlas_ = n ? device_.create_texture(size, size, 1, rhi::TextureFormat::RGBA8_sRGB, mips) : rhi::TextureHandle{};
    for (u32 i = 0; i < n; ++i) {
        std::snprintf(keys_[i], sizeof keys_[i], "%s", wanted[i]);
        times_[i] = times[i];
    }
    count_ = n;
}

void DecalAtlas::apply(const world::World& world, bool skip_hidden, Vec3 eye, rhi::LightSetup& lights) {
    device_.set_decal_atlas(atlas_);
    struct Near {
        f32 distance;
        u32 index;
    };
    std::vector<Near> near;
    const reflect::TypeInfo& type = reflect::type_of<world::Decal>();
    for (u32 i = 0; i < world.count(type); ++i) {
        const world::Entity e = world.owner(type, i);
        if (skip_hidden && world.hidden_in_hierarchy(e)) continue;
        if (tile_of(static_cast<const world::Decal*>(world.at(type, i))->texture) < 0) continue;
        const Mat4 m = world.world_matrix(e);
        near.push_back({length(Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)} - eye), i});
    }
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
    u32 count = 0;
    for (const Near& item : near) {
        if (count == rhi::kMaxDecals) break;
        const auto* d = static_cast<const world::Decal*>(world.at(type, item.index));
        const i32 tile = tile_of(d->texture);
        // World -> box space, the box centred (the editor shows it like the volumes: z from 0 to 1).
        const Mat4 to_box = translation(Vec3{0.0f, 0.0f, -0.5f}) * inverse(world.world_matrix(world.owner(type, item.index)));
        f32 (*rows)[4] = &lights.decals[count * 5];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) rows[r][c] = to_box.at(r, c);
        }
        const f32 tile_size = 1.0f / kTiles;
        rows[3][0] = static_cast<f32>(tile % static_cast<i32>(kTiles)) * tile_size;
        rows[3][1] = static_cast<f32>(tile / static_cast<i32>(kTiles)) * tile_size;
        rows[3][2] = rows[3][3] = tile_size;
        rows[4][0] = d->tint.r;
        rows[4][1] = d->tint.g;
        rows[4][2] = d->tint.b;
        rows[4][3] = d->tint.a;
        ++count;
    }
    lights.decal_info[0] = static_cast<f32>(count);
    // Light functions: their tiles, and how far they have scrolled by now.
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (u32 li = 0; li < lights.count; ++li) {
        const char* guid = nullptr;
        f32 tiling = 1.0f, scroll[2] = {};
        const i32 tile = light_function_of(li, guid, tiling, scroll) ? tile_of(guid) : -1;
        if (tile < 0) continue;
        f32* rect = lights.light_functions[li * 2];
        f32* extra = lights.light_functions[li * 2 + 1];
        rect[0] = static_cast<f32>(tile % static_cast<i32>(kTiles)) / kTiles;
        rect[1] = static_cast<f32>(tile / static_cast<i32>(kTiles)) / kTiles;
        rect[2] = rect[3] = 1.0f / kTiles;
        extra[0] = static_cast<f32>(std::fmod(seconds * scroll[0], 1.0));
        extra[1] = static_cast<f32>(std::fmod(seconds * scroll[1], 1.0));
        extra[2] = tiling;
        extra[3] = 1.0f;
    }
}

} // namespace lsd::editor
