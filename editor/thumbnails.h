#pragma once
// Content Browser thumbnails: each mesh asset rendered once into a small render target.
// shortcut: kept in GPU memory only; save them next to DerivedData when projects get large.
#include "lsd/asset/asset_registry.h"
#include "lsd/foundation/array.h"
#include "lsd/render/mesh.h"

namespace lsd::editor {

class ThumbnailCache {
public:
    ThumbnailCache(rhi::Device& device, Allocator& allocator, const asset::AssetRegistry& registry, const char* data_dir);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // ImTextureID of the asset's thumbnail, or 0 while it is not rendered yet (it is queued).
    // Re-queued automatically when the source file changes.
    u64 get(asset::Guid guid);
    bool flipped() const { return device_.render_targets_flipped(); }

    // Renders up to `budget` queued thumbnails, then switches back to the window.
    // Call between begin_frame and the viewport's draws.
    void render(const f32 background[4], u32 budget = 2);
    // Frees meshes the previous frame's renders used. Call before begin_frame.
    void release_temporaries();

private:
    struct Entry {
        asset::Guid guid;
        rhi::TextureHandle texture;
        i64 source_time = 0;
        bool ready = false;
        bool failed = false;
    };

    bool render_one(Entry& entry, const f32 background[4]);

    rhi::Device& device_;
    Allocator& allocator_;
    const asset::AssetRegistry& registry_;
    char data_dir_[256];
    Array<Entry> entries_;
    Array<render::GpuMesh> temporaries_;
};

} // namespace lsd::editor
