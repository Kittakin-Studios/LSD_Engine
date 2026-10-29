#pragma once
// CPU mesh data, built-in shapes (like Unreal's Basic Shapes) and GPU upload.
#include "lsd/foundation/allocator.h"
#include "lsd/rhi/rhi.h"

namespace lsd::render {

// Vertices and u32 triangle indices in one allocation from `allocator`.
struct MeshData {
    rhi::MeshVertex* vertices = nullptr;
    u32* indices = nullptr;
    u32 vertex_count = 0;
    u32 index_count = 0;
};

MeshData allocate_mesh(Allocator& allocator, u32 vertex_count, u32 index_count);
void free_mesh(Allocator& allocator, MeshData& mesh);

// Shapes sit on the floor (z = 0), 1 unit (1 m) across.
MeshData make_cube(Allocator& allocator);
MeshData make_sphere(Allocator& allocator, u32 segments = 32, u32 rings = 16);
MeshData make_plane(Allocator& allocator);  // 1 x 1, facing +Z, at z = 0

struct GpuMesh {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    u32 index_count = 0;
};

GpuMesh upload_mesh(rhi::Device& device, const MeshData& mesh);
void destroy_mesh(rhi::Device& device, GpuMesh& mesh);

} // namespace lsd::render
