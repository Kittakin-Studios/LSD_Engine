#include "lsd/render/mesh.h"
#include "lsd/foundation/math.h"

#include <cmath>

namespace lsd::render {

MeshData allocate_mesh(Allocator& allocator, u32 vertex_count, u32 index_count) {
    MeshData mesh;
    const usize vertex_bytes = sizeof(rhi::MeshVertex) * vertex_count;
    auto* memory = static_cast<u8*>(allocator.allocate(vertex_bytes + sizeof(u32) * index_count, alignof(rhi::MeshVertex)));
    mesh.vertices = reinterpret_cast<rhi::MeshVertex*>(memory);
    mesh.indices = reinterpret_cast<u32*>(memory + vertex_bytes);
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    return mesh;
}

void free_mesh(Allocator& allocator, MeshData& mesh) {
    if (mesh.vertices) allocator.deallocate(mesh.vertices);
    mesh = {};
}

MeshData make_cube(Allocator& allocator) {
    // 6 faces x 4 vertices so each face has its own flat normal.
    MeshData mesh = allocate_mesh(allocator, 24, 36);
    struct Face { f32 n[3]; f32 u[3]; f32 v[3]; };  // normal and the two in-plane axes (u x v = n)
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}},
    };
    const f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (u32 f = 0; f < 6; ++f) {
        for (u32 c = 0; c < 4; ++c) {
            rhi::MeshVertex& v = mesh.vertices[f * 4 + c];
            for (int k = 0; k < 3; ++k) {
                v.position[k] = 0.5f * (faces[f].n[k] + corners[c][0] * faces[f].u[k] + corners[c][1] * faces[f].v[k]);
                v.normal[k] = faces[f].n[k];
            }
            v.position[2] += 0.5f;  // sit on the floor
            v.uv[0] = corners[c][0] * 0.5f + 0.5f;
            v.uv[1] = 0.5f - corners[c][1] * 0.5f;
        }
        const u32 base = f * 4;
        const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};  // counter-clockwise seen from outside
        for (u32 i = 0; i < 6; ++i) mesh.indices[f * 6 + i] = quad[i];
    }
    return mesh;
}

MeshData make_sphere(Allocator& allocator, u32 segments, u32 rings) {
    MeshData mesh = allocate_mesh(allocator, (segments + 1) * (rings + 1), segments * rings * 6);
    u32 v = 0;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 theta = kPi * static_cast<f32>(r) / static_cast<f32>(rings);  // 0 at the top
        for (u32 s = 0; s <= segments; ++s, ++v) {
            const f32 phi = 2.0f * kPi * static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 n[3] = {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
            rhi::MeshVertex& out = mesh.vertices[v];
            for (int k = 0; k < 3; ++k) {
                out.normal[k] = n[k];
                out.position[k] = 0.5f * n[k];
            }
            out.position[2] += 0.5f;
            out.uv[0] = static_cast<f32>(s) / static_cast<f32>(segments);
            out.uv[1] = static_cast<f32>(r) / static_cast<f32>(rings);
        }
    }
    u32 i = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            const u32 quad[6] = {a, b, b + 1, a, b + 1, a + 1};
            for (u32 q : quad) mesh.indices[i++] = q;
        }
    }
    return mesh;
}

MeshData make_plane(Allocator& allocator) {
    MeshData mesh = allocate_mesh(allocator, 4, 6);
    const f32 corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (u32 c = 0; c < 4; ++c) {
        mesh.vertices[c] = {{corners[c][0], corners[c][1], 0.0f}, {0, 0, 1}, {corners[c][0] + 0.5f, 0.5f - corners[c][1]}};
    }
    const u32 indices[6] = {0, 1, 2, 0, 2, 3};
    for (u32 i = 0; i < 6; ++i) mesh.indices[i] = indices[i];
    return mesh;
}

GpuMesh upload_mesh(rhi::Device& device, const MeshData& mesh) {
    GpuMesh gpu;
    gpu.vertices = device.create_buffer(rhi::BufferUsage::Vertex, mesh.vertices, sizeof(rhi::MeshVertex) * mesh.vertex_count);
    gpu.indices = device.create_buffer(rhi::BufferUsage::Index, mesh.indices, sizeof(u32) * mesh.index_count);
    gpu.index_count = mesh.index_count;
    return gpu;
}

void destroy_mesh(rhi::Device& device, GpuMesh& mesh) {
    device.destroy_buffer(mesh.vertices);
    device.destroy_buffer(mesh.indices);
    mesh = {};
}

} // namespace lsd::render
