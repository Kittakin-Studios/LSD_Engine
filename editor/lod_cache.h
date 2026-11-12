#pragma once
// Generated LODs kept in DerivedData beside the compiled mesh ("<compiled>.lod"): a mesh loaded again reads them
// instead of simplifying it again (most of a dense mesh's load time), and the background compiler builds them on a
// worker thread so the first load does not either. Stale when the compiled mesh or the LOD screen sizes changed.
#include "lsd/render/lod.h"
#include "lsd/render/mesh.h"

namespace lsd::editor {

struct LodSet {
    u32 lod_count = 1;                         // 1: no generated LODs
    u32 counts[render::kMaxLods - 1] = {};     // LOD k's index count at [k - 1]
};

// The cache for this mesh (compiled at `compiled_time`, file time) and these screen sizes, into out[k - 1] (each
// with room for mesh.index_count). False when missing, stale or damaged.
bool read_lod_cache(const char* path, i64 compiled_time, const render::MeshData& mesh, const f32 sizes[render::kMaxLods], u32* const out[render::kMaxLods - 1],
                    LodSet& lods);
// Writes it (beside and renamed over: safe while another thread reads it).
void write_lod_cache(const char* path, i64 compiled_time, const render::MeshData& mesh, const f32 sizes[render::kMaxLods], u32* const out[render::kMaxLods - 1],
                     const LodSet& lods);
// Only the header: current for that compiled file and those sizes (the main thread's cheap check).
bool lod_cache_current(const char* path, i64 compiled_time, const f32 sizes[render::kMaxLods]);
// For a compiled mesh: builds its LODs and writes the cache unless it is current or the mesh has its own LODs.
// No GPU, no registry: safe on a worker thread.
bool warm_lod_cache(const char* compiled, const f32 sizes[render::kMaxLods], Allocator& allocator);

} // namespace lsd::editor
