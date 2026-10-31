#pragma once
// Decal components for the renderer: their textures packed into one atlas (4 x 4 tiles of 256), and
// the decals nearest the camera written into a LightSetup (rhi::kMaxDecals).
// shortcut: 16 textures at 256 px, no mips; a texture array when decals need detail or count.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/allocator.h"
#include "lsd/rhi/rhi.h"
#include "lsd/world/world.h"

namespace lsd::editor {

class DecalAtlas {
public:
    DecalAtlas(rhi::Device& device, Allocator& allocator, const asset::AssetRegistry& registry) : device_(device), allocator_(allocator), registry_(registry) {}
    ~DecalAtlas();
    DecalAtlas(const DecalAtlas&) = delete;
    DecalAtlas& operator=(const DecalAtlas&) = delete;

    // Rebuilds the atlas when the level's decal textures (or their files) change.
    void update(const world::World& world);
    // The decals near `eye` into `lights`, and the atlas onto the device.
    void apply(const world::World& world, bool skip_hidden, Vec3 eye, rhi::LightSetup& lights);

private:
    static constexpr u32 kTile = 256, kTiles = 4;
    rhi::Device& device_;
    Allocator& allocator_;
    const asset::AssetRegistry& registry_;
    rhi::TextureHandle atlas_;
    char keys_[kTiles * kTiles][33] = {};  // texture GUID per tile
    i64 times_[kTiles * kTiles] = {};
    u32 count_ = 0;
    i32 tile_of(const char* guid) const;
};

} // namespace lsd::editor
