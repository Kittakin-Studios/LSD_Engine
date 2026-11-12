#pragma once
// Background asset compiles (Unreal's Derived Data Cache build / shader compile workers): meshes into
// DerivedData/<guid>.lsdmesh and textures into DerivedData/Textures on a pool of worker threads, so a
// project opened for the first time stays responsive while its meshes compile, and Build > Compile All
// Assets warms DerivedData for the whole project before a playtest.
#include "lsd/asset/asset_registry.h"

namespace lsd::editor::compiler {

// A mesh key (a mesh asset's GUID or "<guid>#<part>") whose compiled file is missing or stale: queued, and
// true until it is done (draw nothing for it meanwhile). False when it is up to date or not a mesh asset.
// Main thread (it reads the registry); the import itself runs on a worker.
bool queue_mesh(const asset::AssetRegistry& registry, const char* key, const char* data_dir);
// Still queued or compiling.
bool mesh_busy(const char* key, const char* data_dir);

// A texture as a material uses it (colour = sRGB, normal map), into the same cache file TextureCache reads.
void queue_texture(const char* path, bool srgb, bool normal_map, u32 max_size);

// Every mesh asset, and every texture the project's materials use, that is not compiled yet. Returns how many
// were queued. `compress`: the device takes BC textures (as TextureCache compresses them).
u32 compile_all(const asset::AssetRegistry& registry, const char* data_dir, bool compress);

struct Progress {
    u32 done = 0, total = 0;  // since the queue was last empty
    u32 failed = 0;
};
Progress progress();
bool busy();
void wait();      // until the queue is empty (Play, packaging)
void shutdown();  // stops the workers (editor exit)

} // namespace lsd::editor::compiler
